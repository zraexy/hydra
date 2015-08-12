#include "state.hh"
#include "build-result.hh"

using namespace nix;


void State::queueMonitor()
{
    while (true) {
        try {
            queueMonitorLoop();
        } catch (std::exception & e) {
            printMsg(lvlError, format("queue monitor: %1%") % e.what());
            sleep(10); // probably a DB problem, so don't retry right away
        }
    }
}


void State::queueMonitorLoop()
{
    auto conn(dbPool.get());

    receiver buildsAdded(*conn, "builds_added");
    receiver buildsRestarted(*conn, "builds_restarted");
    receiver buildsCancelled(*conn, "builds_cancelled");
    receiver buildsDeleted(*conn, "builds_deleted");
    receiver buildsBumped(*conn, "builds_bumped");

    auto store = openStore(); // FIXME: pool

    unsigned int lastBuildId = 0;

    while (true) {
        getQueuedBuilds(*conn, store, lastBuildId);

        /* Sleep until we get notification from the database about an
           event. */
        conn->await_notification();
        nrQueueWakeups++;

        if (buildsAdded.get())
            printMsg(lvlTalkative, "got notification: new builds added to the queue");
        if (buildsRestarted.get()) {
            printMsg(lvlTalkative, "got notification: builds restarted");
            lastBuildId = 0; // check all builds
        }
        if (buildsCancelled.get() || buildsDeleted.get() || buildsBumped.get()) {
            printMsg(lvlTalkative, "got notification: builds cancelled or bumped");
            processQueueChange(*conn);
        }
    }
}


void State::getQueuedBuilds(Connection & conn, std::shared_ptr<StoreAPI> store, unsigned int & lastBuildId)
{
    printMsg(lvlInfo, format("checking the queue for builds > %1%...") % lastBuildId);

    /* Grab the queued builds from the database, but don't process
       them yet (since we don't want a long-running transaction). */
    std::vector<BuildID> newIDs;
    std::map<BuildID, Build::ptr> newBuildsByID;
    std::multimap<Path, BuildID> newBuildsByPath;

    {
        pqxx::work txn(conn);

        auto res = txn.parameterized
            ("select id, project, jobset, job, drvPath, maxsilent, timeout, timestamp, globalPriority, priority from Builds "
             "where id > $1 and finished = 0 order by globalPriority desc, id")
            (lastBuildId).exec();

        for (auto const & row : res) {
            auto builds_(builds.lock());
            BuildID id = row["id"].as<BuildID>();
            if (buildOne && id != buildOne) continue;
            if (id > lastBuildId) lastBuildId = id;
            if (has(*builds_, id)) continue;

            auto build = std::make_shared<Build>();
            build->id = id;
            build->drvPath = row["drvPath"].as<string>();
            build->projectName = row["project"].as<string>();
            build->jobsetName = row["jobset"].as<string>();
            build->jobName = row["job"].as<string>();
            build->maxSilentTime = row["maxsilent"].as<int>();
            build->buildTimeout = row["timeout"].as<int>();
            build->timestamp = row["timestamp"].as<time_t>();
            build->globalPriority = row["globalPriority"].as<int>();
            build->localPriority = row["priority"].as<int>();
            build->jobset = createJobset(txn, build->projectName, build->jobsetName);

            newIDs.push_back(id);
            newBuildsByID[id] = build;
            newBuildsByPath.emplace(std::make_pair(build->drvPath, id));
        }
    }

    std::set<Step::ptr> newRunnable;
    unsigned int nrAdded;
    std::function<void(Build::ptr)> createBuild;

    createBuild = [&](Build::ptr build) {
        printMsg(lvlTalkative, format("loading build %1% (%2%)") % build->id % build->fullJobName());
        nrAdded++;
        newBuildsByID.erase(build->id);

        if (!store->isValidPath(build->drvPath)) {
            /* Derivation has been GC'ed prematurely. */
            printMsg(lvlError, format("aborting GC'ed build %1%") % build->id);
            if (!build->finishedInDB) {
                pqxx::work txn(conn);
                txn.parameterized
                    ("update Builds set finished = 1, busy = 0, buildStatus = $2, startTime = $3, stopTime = $3, errorMsg = $4 where id = $1 and finished = 0")
                    (build->id)
                    ((int) bsAborted)
                    (time(0))
                    ("derivation was garbage-collected prior to build").exec();
                txn.commit();
                build->finishedInDB = true;
                nrBuildsDone++;
            }
            return;
        }

        std::set<Step::ptr> newSteps;
        std::set<Path> finishedDrvs; // FIXME: re-use?
        Step::ptr step = createStep(store, build->drvPath, build, 0, finishedDrvs, newSteps, newRunnable);

        /* Some of the new steps may be the top level of builds that
           we haven't processed yet. So do them now. This ensures that
           if build A depends on build B with top-level step X, then X
           will be "accounted" to B in doBuildStep(). */
        for (auto & r : newSteps) {
            auto i = newBuildsByPath.find(r->drvPath);
            if (i == newBuildsByPath.end()) continue;
            auto j = newBuildsByID.find(i->second);
            if (j == newBuildsByID.end()) continue;
            createBuild(j->second);
        }

        /* If we didn't get a step, it means the step's outputs are
           all valid. So we mark this as a finished, cached build. */
        if (!step) {
            Derivation drv = readDerivation(build->drvPath);
            BuildOutput res = getBuildOutput(store, drv);

            pqxx::work txn(conn);
            time_t now = time(0);
            markSucceededBuild(txn, build, res, true, now, now);
            txn.commit();

            build->finishedInDB = true;

            return;
        }

        /* If any step has an unsupported system type or has a
           previously failed output path, then fail the build right
           away. */
        bool badStep = false;
        for (auto & r : newSteps) {
            BuildStatus buildStatus = bsSuccess;
            BuildStepStatus buildStepStatus = bssFailed;

            if (checkCachedFailure(r, conn)) {
                printMsg(lvlError, format("marking build %1% as cached failure") % build->id);
                buildStatus = step == r ? bsFailed : bsDepFailed;
                buildStepStatus = bssFailed;
            }

            if (buildStatus == bsSuccess) {
                bool supported = false;
                {
                    auto machines_(machines.lock()); // FIXME: use shared_mutex
                    for (auto & m : *machines_)
                        if (m.second->supportsStep(r)) { supported = true; break; }
                }

                if (!supported) {
                    printMsg(lvlError, format("aborting unsupported build %1%") % build->id);
                    buildStatus = bsUnsupported;
                    buildStepStatus = bssUnsupported;
                }
            }

            if (buildStatus != bsSuccess) {
                time_t now = time(0);
                if (!build->finishedInDB) {
                    pqxx::work txn(conn);
                    createBuildStep(txn, 0, build, r, "", buildStepStatus);
                    txn.parameterized
                        ("update Builds set finished = 1, busy = 0, buildStatus = $2, startTime = $3, stopTime = $3, isCachedBuild = $4 where id = $1 and finished = 0")
                        (build->id)
                        ((int) buildStatus)
                        (now)
                        (buildStatus != bsUnsupported ? 1 : 0).exec();
                    txn.commit();
                    build->finishedInDB = true;
                    nrBuildsDone++;
                }
                badStep = true;
                break;
            }
        }

        if (badStep) return;

        /* Note: if we exit this scope prior to this, the build and
           all newly created steps are destroyed. */

        {
            auto builds_(builds.lock());
            if (!build->finishedInDB) // FIXME: can this happen?
                (*builds_)[build->id] = build;
            build->toplevel = step;
        }

        build->propagatePriorities();

        printMsg(lvlChatty, format("added build %1% (top-level step %2%, %3% new steps)")
            % build->id % step->drvPath % newSteps.size());
    };

    /* Now instantiate build steps for each new build. The builder
       threads can start building the runnable build steps right away,
       even while we're still processing other new builds. */
    for (auto id : newIDs) {
        auto i = newBuildsByID.find(id);
        if (i == newBuildsByID.end()) continue;
        auto build = i->second;

        newRunnable.clear();
        nrAdded = 0;
        try {
            createBuild(build);
        } catch (Error & e) {
            e.addPrefix(format("while loading build %1%: ") % build->id);
            throw;
        }

        /* Add the new runnable build steps to ‘runnable’ and wake up
           the builder threads. */
        printMsg(lvlChatty, format("got %1% new runnable steps from %2% new builds") % newRunnable.size() % nrAdded);
        for (auto & r : newRunnable)
            makeRunnable(r);

        nrBuildsRead += nrAdded;
    }
}


void Build::propagatePriorities()
{
    /* Update the highest global priority and lowest build ID fields
       of each dependency. This is used by the dispatcher to start
       steps in order of descending global priority and ascending
       build ID. */
    visitDependencies([&](const Step::ptr & step) {
        auto step_(step->state.lock());
        step_->highestGlobalPriority = std::max(step_->highestGlobalPriority, globalPriority);
        step_->highestLocalPriority = std::max(step_->highestLocalPriority, localPriority);
        step_->lowestBuildID = std::min(step_->lowestBuildID, id);
        step_->jobsets.insert(jobset);
    }, toplevel);
}


void State::processQueueChange(Connection & conn)
{
    /* Get the current set of queued builds. */
    std::map<BuildID, int> currentIds;
    {
        pqxx::work txn(conn);
        auto res = txn.exec("select id, globalPriority from Builds where finished = 0");
        for (auto const & row : res)
            currentIds[row["id"].as<BuildID>()] = row["globalPriority"].as<BuildID>();
    }

    auto builds_(builds.lock());

    for (auto i = builds_->begin(); i != builds_->end(); ) {
        auto b = currentIds.find(i->first);
        if (b == currentIds.end()) {
            printMsg(lvlInfo, format("discarding cancelled build %1%") % i->first);
            i = builds_->erase(i);
            // FIXME: ideally we would interrupt active build steps here.
            continue;
        }
        if (i->second->globalPriority < b->second) {
            printMsg(lvlInfo, format("priority of build %1% increased") % i->first);
            i->second->globalPriority = b->second;
            i->second->propagatePriorities();
        }
        ++i;
    }
}


Step::ptr State::createStep(std::shared_ptr<StoreAPI> store, const Path & drvPath,
    Build::ptr referringBuild, Step::ptr referringStep, std::set<Path> & finishedDrvs,
    std::set<Step::ptr> & newSteps, std::set<Step::ptr> & newRunnable)
{
    if (finishedDrvs.find(drvPath) != finishedDrvs.end()) return 0;

    /* Check if the requested step already exists. If not, create a
       new step. In any case, make the step reachable from
       referringBuild or referringStep. This is done atomically (with
       ‘steps’ locked), to ensure that this step can never become
       reachable from a new build after doBuildStep has removed it
       from ‘steps’. */
    Step::ptr step;
    bool isNew = false;
    {
        auto steps_(steps.lock());

        /* See if the step already exists in ‘steps’ and is not
           stale. */
        auto prev = steps_->find(drvPath);
        if (prev != steps_->end()) {
            step = prev->second.lock();
            /* Since ‘step’ is a strong pointer, the referred Step
               object won't be deleted after this. */
            if (!step) steps_->erase(drvPath); // remove stale entry
        }

        /* If it doesn't exist, create it. */
        if (!step) {
            step = std::make_shared<Step>();
            step->drvPath = drvPath;
            isNew = true;
        }

        auto step_(step->state.lock());

        assert(step_->created != isNew);

        if (referringBuild)
            step_->builds.push_back(referringBuild);

        if (referringStep)
            step_->rdeps.push_back(referringStep);

        (*steps_)[drvPath] = step;
    }

    if (!isNew) return step;

    printMsg(lvlDebug, format("considering derivation ‘%1%’") % drvPath);

    /* Initialize the step. Note that the step may be visible in
       ‘steps’ before this point, but that doesn't matter because
       it's not runnable yet, and other threads won't make it
       runnable while step->created == false. */
    step->drv = readDerivation(drvPath);
    {
        auto i = step->drv.env.find("requiredSystemFeatures");
        if (i != step->drv.env.end())
            step->requiredSystemFeatures = tokenizeString<std::set<std::string>>(i->second);
    }

    auto attr = step->drv.env.find("preferLocalBuild");
    step->preferLocalBuild =
        attr != step->drv.env.end() && attr->second == "1"
        && has(localPlatforms, step->drv.platform);

    /* Are all outputs valid? */
    bool valid = true;
    for (auto & i : step->drv.outputs) {
        if (!store->isValidPath(i.second.path)) {
            valid = false;
            break;
        }
    }

    // FIXME: check whether all outputs are in the binary cache.
    if (valid) {
        finishedDrvs.insert(drvPath);
        return 0;
    }

    /* No, we need to build. */
    printMsg(lvlDebug, format("creating build step ‘%1%’") % drvPath);
    newSteps.insert(step);

    /* Create steps for the dependencies. */
    for (auto & i : step->drv.inputDrvs) {
        auto dep = createStep(store, i.first, 0, step, finishedDrvs, newSteps, newRunnable);
        if (dep) {
            auto step_(step->state.lock());
            step_->deps.insert(dep);
        }
    }

    /* If the step has no (remaining) dependencies, make it
       runnable. */
    {
        auto step_(step->state.lock());
        assert(!step_->created);
        step_->created = true;
        if (step_->deps.empty())
            newRunnable.insert(step);
    }

    return step;
}


Jobset::ptr State::createJobset(pqxx::work & txn,
    const std::string & projectName, const std::string & jobsetName)
{
    auto jobsets_(jobsets.lock());

    auto p = std::make_pair(projectName, jobsetName);

    auto i = jobsets_->find(p);
    if (i != jobsets_->end()) return i->second;

    auto res = txn.parameterized
        ("select schedulingShares from Jobsets where project = $1 and name = $2")
        (projectName)(jobsetName).exec();
    if (res.empty()) throw Error("missing jobset - can't happen");

    auto shares = res[0]["schedulingShares"].as<unsigned int>();
    if (shares == 0) shares = 1;

    auto jobset = std::make_shared<Jobset>(shares);

    /* Load the build steps from the last 24 hours. */
    res = txn.parameterized
        ("select s.startTime, s.stopTime from BuildSteps s join Builds b on build = id "
         "where s.startTime is not null and s.stopTime > $1 and project = $2 and jobset = $3")
        (time(0) - Jobset::schedulingWindow * 10)(projectName)(jobsetName).exec();
    for (auto const & row : res) {
        time_t startTime = row["startTime"].as<time_t>();
        time_t stopTime = row["stopTime"].as<time_t>();
        jobset->addStep(startTime, stopTime - startTime);
    }

    (*jobsets_)[p] = jobset;
    return jobset;
}
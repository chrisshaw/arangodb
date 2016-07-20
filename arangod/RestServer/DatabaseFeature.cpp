////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2016 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "DatabaseFeature.h"

#include "ApplicationFeatures/ApplicationServer.h"
#include "Aql/QueryRegistry.h"
#include "Basics/FileUtils.h"
#include "Basics/MutexLocker.h"
#include "Basics/StringUtils.h"
#include "Basics/files.h"
#include "Cluster/ServerState.h"
#include "Cluster/v8-cluster.h"
#include "Logger/Logger.h"
#include "ProgramOptions/ProgramOptions.h"
#include "ProgramOptions/Section.h"
#include "RestServer/DatabaseFeature.h"
#include "RestServer/DatabasePathFeature.h"
#include "RestServer/QueryRegistryFeature.h"
#include "RestServer/RestServerFeature.h"
#include "StorageEngine/EngineSelectorFeature.h"
#include "StorageEngine/StorageEngine.h"
#include "Utils/CursorRepository.h"
#include "V8Server/V8DealerFeature.h"
#include "V8Server/v8-query.h"
#include "V8Server/v8-vocbase.h"
#include "VocBase/KeyGenerator.h"
#include "VocBase/replication-applier.h"
#include "VocBase/server.h"
#include "VocBase/vocbase.h"
#include "Wal/LogfileManager.h"

#ifdef ARANGODB_ENABLE_ROCKSDB
#include "Indexes/RocksDBIndex.h"
#endif

#include <velocypack/velocypack-aliases.h>

using namespace arangodb;
using namespace arangodb::application_features;
using namespace arangodb::basics;
using namespace arangodb::options;

TRI_server_t* DatabaseFeature::SERVER = nullptr;

uint32_t const DatabaseFeature::DefaultIndexBuckets = 8;

DatabaseFeature* DatabaseFeature::DATABASE = nullptr;

/// @brief database manager thread main loop
/// the purpose of this thread is to physically remove directories of databases
/// that have been dropped
DatabaseManagerThread::DatabaseManagerThread()
    : Thread("DatabaseManager") {}

DatabaseManagerThread::~DatabaseManagerThread() {}

void DatabaseManagerThread::run() {
  auto databaseFeature = ApplicationServer::getFeature<DatabaseFeature>("Database");
  auto dealer = ApplicationServer::getFeature<V8DealerFeature>("V8Dealer");
  int cleanupCycles = 0;

  while (true) {

    // check if we have to drop some database
    TRI_vocbase_t* database = nullptr;

    {
      auto unuser(databaseFeature->_databasesProtector.use());
      auto theLists = databaseFeature->_databasesLists.load();

      for (TRI_vocbase_t* vocbase : theLists->_droppedDatabases) {
        if (!TRI_CanRemoveVocBase(vocbase)) {
          continue;
        }

        // found a database to delete
        database = vocbase;
        break;
      }
    }

    if (database != nullptr) {
      // found a database to delete, now remove it from the struct

      {
        MUTEX_LOCKER(mutexLocker, databaseFeature->_databasesMutex);

        // Build the new value:
        auto oldLists = databaseFeature->_databasesLists.load();
        decltype(oldLists) newLists = nullptr;
        try {
          newLists = new DatabasesLists();
          newLists->_databases = oldLists->_databases;
          newLists->_coordinatorDatabases = oldLists->_coordinatorDatabases;
          for (TRI_vocbase_t* vocbase : oldLists->_droppedDatabases) {
            if (vocbase != database) {
              newLists->_droppedDatabases.insert(vocbase);
            }
          }
        } catch (...) {
          delete newLists;
          continue;  // try again later
        }

        // Replace the old by the new:
        databaseFeature->_databasesLists = newLists;
        databaseFeature->_databasesProtector.scan();
        delete oldLists;

        // From now on no other thread can possibly see the old TRI_vocbase_t*,
        // note that there is only one DatabaseManager thread, so it is
        // not possible that another thread has seen this very database
        // and tries to free it at the same time!
      }

      if (database->_type != TRI_VOCBASE_TYPE_COORDINATOR) {
        // regular database
        // ---------------------------

#ifdef ARANGODB_ENABLE_ROCKSDB
        // delete persistent indexes for this database
        RocksDBFeature::dropDatabase(database->_id);
#endif

        LOG(TRACE) << "physically removing database directory '"
                    << database->_path << "' of database '" << database->_name
                    << "'";

        std::string path;

        // remove apps directory for database
        auto appPath = dealer->appPath();

        if (database->_isOwnAppsDirectory && !appPath.empty()) {
          path = arangodb::basics::FileUtils::buildFilename(
              arangodb::basics::FileUtils::buildFilename(appPath, "_db"),
              database->_name);

          if (TRI_IsDirectory(path.c_str())) {
            LOG(TRACE) << "removing app directory '" << path
                      << "' of database '" << database->_name << "'";

            TRI_RemoveDirectory(path.c_str());
          }
        }

        // remember db path
        path = std::string(database->_path);

        TRI_DestroyVocBase(database);

        // remove directory
        TRI_RemoveDirectory(path.c_str());
      }

      delete database;

      // directly start next iteration
    } else {
      if (isStopping()) {
        // done
        break;
      }

      usleep(500 * 1000); // TODO
      // The following is only necessary after a wait:
      auto queryRegistry = databaseFeature->_queryRegistry.load();

      if (queryRegistry != nullptr) {
        queryRegistry->expireQueries();
      }

      // on a coordinator, we have no cleanup threads for the databases
      // so we have to do cursor cleanup here
      if (++cleanupCycles >= 10 &&
          arangodb::ServerState::instance()->isCoordinator()) {
        // note: if no coordinator then cleanupCycles will increase endlessly,
        // but it's only used for the following part
        cleanupCycles = 0;

        auto unuser(databaseFeature->_databasesProtector.use());
        auto theLists = databaseFeature->_databasesLists.load();

        for (auto& p : theLists->_coordinatorDatabases) {
          TRI_vocbase_t* vocbase = p.second;
          TRI_ASSERT(vocbase != nullptr);
          auto cursorRepository = vocbase->_cursorRepository;

          try {
            cursorRepository->garbageCollect(false);
          } catch (...) {
          }
        }
      }
    }

    // next iteration
  }
}

DatabaseFeature::DatabaseFeature(ApplicationServer* server)
    : ApplicationFeature(server, "Database"),
      _maximalJournalSize(TRI_JOURNAL_DEFAULT_MAXIMAL_SIZE),
      _defaultWaitForSync(false),
      _forceSyncProperties(true),
      _ignoreDatafileErrors(false),
      _throwCollectionNotLoadedError(false),
      _server(),
      _vocbase(nullptr),
      _queryRegistry(nullptr),
      _databaseManager(nullptr),
      _databasesLists(new DatabasesLists()),
      _isInitiallyEmpty(false),
      _replicationApplier(true),
      _disableCompactor(false),
      _checkVersion(false),
      _iterateMarkersOnOpen(false),
      _upgrade(false) {
  setOptional(false);
  requiresElevatedPrivileges(false);
  startsAfter("DatabasePath");
  startsAfter("LogfileManager");
  startsAfter("InitDatabase");
  startsAfter("IndexPool");
}

DatabaseFeature::~DatabaseFeature() {
  delete _databaseManager;

  try {
    // closeOpenDatabases() can throw, but we're in a dtor
    closeOpenDatabases();
  } catch (...) {
  }

  auto p = _databasesLists.load();
  delete p;
}

void DatabaseFeature::collectOptions(std::shared_ptr<ProgramOptions> options) {
  options->addSection("database", "Configure the database");

  options->addOldOption("server.disable-replication-applier",
                        "database.replication-applier");

  options->addOption("--database.maximal-journal-size",
                     "default maximal journal size, can be overwritten when "
                     "creating a collection",
                     new UInt64Parameter(&_maximalJournalSize));

  options->addHiddenOption("--database.wait-for-sync",
                           "default wait-for-sync behavior, can be overwritten "
                           "when creating a collection",
                           new BooleanParameter(&_defaultWaitForSync));

  options->addHiddenOption("--database.force-sync-properties",
                           "force syncing of collection properties to disk, "
                           "will use waitForSync value of collection when "
                           "turned off",
                           new BooleanParameter(&_forceSyncProperties));

  options->addHiddenOption(
      "--database.ignore-datafile-errors",
      "load collections even if datafiles may contain errors",
      new BooleanParameter(&_ignoreDatafileErrors));

  options->addHiddenOption(
      "--database.throw-collection-not-loaded-error",
      "throw an error when accessing a collection that is still loading",
      new BooleanParameter(&_throwCollectionNotLoadedError));

  options->addHiddenOption(
      "--database.replication-applier",
      "switch to enable or disable the replication applier",
      new BooleanParameter(&_replicationApplier));
}

void DatabaseFeature::validateOptions(std::shared_ptr<ProgramOptions> options) {
  if (_maximalJournalSize < TRI_JOURNAL_MINIMAL_SIZE) {
    LOG(FATAL) << "invalid value for '--database.maximal-journal-size'. "
                  "expected at least "
               << TRI_JOURNAL_MINIMAL_SIZE;
    FATAL_ERROR_EXIT();
  }
  
  // sanity check
  if (_checkVersion && _upgrade) {
    LOG(FATAL) << "cannot specify both '--database.check-version' and "
                  "'--database.auto-upgrade'";
    FATAL_ERROR_EXIT();
  }
}

void DatabaseFeature::prepare() {
  // create the server
  _server.reset(new TRI_server_t()); // TODO
  SERVER = _server.get();
}

void DatabaseFeature::start() {
  // set singleton
  DATABASE = this;

  // set throw collection not loaded behavior
  TRI_SetThrowCollectionNotLoadedVocBase(_throwCollectionNotLoadedError);

  // init key generator
  KeyGenerator::Initialize();

  // initialize storage engine
  StorageEngine* engine = ApplicationServer::getFeature<EngineSelectorFeature>("EngineSelector")->ENGINE;
  engine->initialize();

  _iterateMarkersOnOpen = !wal::LogfileManager::instance()->hasFoundLastTick();

  // create shared application directory js/apps
  V8DealerFeature* dealer = ApplicationServer::getFeature<V8DealerFeature>("V8Dealer");
  auto appPath = dealer->appPath();

  if (!appPath.empty() && !TRI_IsDirectory(appPath.c_str())) {
    long systemError;
    std::string errorMessage;
    int res = TRI_CreateRecursiveDirectory(appPath.c_str(), systemError,
                                           errorMessage);

    if (res == TRI_ERROR_NO_ERROR) {
      LOG(INFO) << "created --javascript.app-path directory '" << appPath << "'";
    } else {
      LOG(ERR) << "unable to create --javascript.app-path directory '"
               << appPath << "': " << errorMessage;
      THROW_ARANGO_EXCEPTION(res);
    }
  }
  
  // create subdirectory js/apps/_db if not yet present
  int res = createBaseApplicationDirectory(appPath, "_db");

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(ERR) << "unable to initialize databases: " << TRI_errno_string(res);
    THROW_ARANGO_EXCEPTION(res);
  }

  // scan all databases
  VPackBuilder builder;
  engine->getDatabases(builder);

  TRI_ASSERT(builder.slice().isArray());

  res = iterateDatabases(builder.slice());

  if (res != TRI_ERROR_NO_ERROR) {
    LOG(ERR) << "could not iterate over all databases: " << TRI_errno_string(res);
    THROW_ARANGO_EXCEPTION(res);
  }

  // start database manager thread
  _databaseManager = new DatabaseManagerThread;
    
  if (!_databaseManager->start()) {
    LOG(FATAL) << "could not start database manager thread";
    FATAL_ERROR_EXIT();
  }

  if (res != TRI_ERROR_NO_ERROR) {
    if (res == TRI_ERROR_ARANGO_EMPTY_DATADIR) {
      LOG_TOPIC(TRACE, Logger::STARTUP) << "database is empty";
      _isInitiallyEmpty = true;
    }

    if (! _checkVersion || ! _isInitiallyEmpty) {
      LOG(FATAL) << "cannot start server: " << TRI_errno_string(res);
      FATAL_ERROR_EXIT();
    }
  }

  if (!_isInitiallyEmpty) {
    LOG_TOPIC(TRACE, Logger::STARTUP) << "found system database";
  }

  if (_isInitiallyEmpty && _checkVersion) {
    LOG(DEBUG) << "checking version on an empty database";
    TRI_EXIT_FUNCTION(EXIT_SUCCESS, nullptr);
  }

  // update all v8 contexts
  updateContexts();

  // active deadlock detection in case we're not running in cluster mode
  if (!arangodb::ServerState::instance()->isRunningInCluster()) {
    TRI_EnableDeadlockDetectionDatabasesServer(DatabaseFeature::SERVER);
  }
}

void DatabaseFeature::unprepare() {
  // close all databases
  closeDatabases();

  // delete the server
  _databaseManager->beginShutdown();

  while (_databaseManager->isRunning()) {
    usleep(1000);
  }
 
  try {   
    closeDroppedDatabases();
  } catch (...) {
    // we're in the shutdown... simply ignore any errors produced here
  }
  
  StorageEngine* engine = ApplicationServer::getFeature<EngineSelectorFeature>("EngineSelector")->ENGINE;
  engine->shutdown();
  
  // clear singleton
  DATABASE = nullptr;
  SERVER = nullptr;
  _server.reset(nullptr);
}

void DatabaseFeature::updateContexts() {
  _vocbase = TRI_UseDatabaseServer(DatabaseFeature::SERVER,
                                   TRI_VOC_SYSTEM_DATABASE);

  if (_vocbase == nullptr) {
    LOG(FATAL)
        << "No _system database found in database directory. Cannot start!";
    FATAL_ERROR_EXIT();
  }

  auto queryRegistry = QueryRegistryFeature::QUERY_REGISTRY;
  TRI_ASSERT(queryRegistry != nullptr);

  auto server = DatabaseFeature::SERVER;
  TRI_ASSERT(server != nullptr);

  auto vocbase = _vocbase;

  V8DealerFeature* dealer =
      ApplicationServer::getFeature<V8DealerFeature>("V8Dealer");

  dealer->defineContextUpdate(
      [queryRegistry, server, vocbase](
          v8::Isolate* isolate, v8::Handle<v8::Context> context, size_t i) {
        TRI_InitV8VocBridge(isolate, context, queryRegistry, server, vocbase,
                            i);
        TRI_InitV8Queries(isolate, context);
        TRI_InitV8Cluster(isolate, context);
      },
      vocbase);
}

void DatabaseFeature::shutdownCompactor() {
  auto unuser = DatabaseFeature::SERVER->_databasesProtector.use();
  auto theLists = DatabaseFeature::SERVER->_databasesLists.load();

  for (auto& p : theLists->_databases) {
    TRI_vocbase_t* vocbase = p.second;

    vocbase->_state = 2;

    int res = TRI_ERROR_NO_ERROR;

    res |= TRI_StopCompactorVocBase(vocbase);
    vocbase->_state = 3;
    res |= TRI_JoinThread(&vocbase->_cleanup);

    if (res != TRI_ERROR_NO_ERROR) {
      LOG(ERR) << "unable to join database threads for database '"
               << vocbase->_name << "'";
    }
  }
}

void DatabaseFeature::closeDatabases() {
  // stop the replication appliers so all replication transactions can end
  if (_replicationApplier) {
    MUTEX_LOCKER(mutexLocker, _databasesMutex);  // Only one should do this at a time
    // No need for the thread protector here, because we have the mutex

    for (auto& p : _databasesLists.load()->_databases) {
      TRI_vocbase_t* vocbase = p.second;
      TRI_ASSERT(vocbase != nullptr);
      TRI_ASSERT(vocbase->_type == TRI_VOCBASE_TYPE_NORMAL);
      if (vocbase->_replicationApplier != nullptr) {
        vocbase->_replicationApplier->stop(false);
      }
    }
  }
}

/// @brief close all opened databases
void DatabaseFeature::closeOpenDatabases() {
  MUTEX_LOCKER(mutexLocker, _databasesMutex);  // Only one should do this at a time
  // No need for the thread protector here, because we have the mutex
  // Note however, that somebody could still read the lists concurrently,
  // therefore we first install a new value, call scan() on the protector
  // and only then really destroy the vocbases:

  // Build the new value:
  auto oldList = _databasesLists.load();
  decltype(oldList) newList = nullptr;
  try {
    newList = new DatabasesLists();
    newList->_droppedDatabases = _databasesLists.load()->_droppedDatabases;
  } catch (...) {
    delete newList;
    throw;
  }

  // Replace the old by the new:
  _databasesLists = newList;
  _databasesProtector.scan();

  // Now it is safe to destroy the old databases and the old lists struct:
  for (auto& p : oldList->_databases) {
    TRI_vocbase_t* vocbase = p.second;
    TRI_ASSERT(vocbase != nullptr);
    TRI_ASSERT(vocbase->_type == TRI_VOCBASE_TYPE_NORMAL);
    TRI_DestroyVocBase(vocbase);

    delete vocbase;
  }

  for (auto& p : oldList->_coordinatorDatabases) {
    TRI_vocbase_t* vocbase = p.second;
    TRI_ASSERT(vocbase != nullptr);
    TRI_ASSERT(vocbase->_type == TRI_VOCBASE_TYPE_COORDINATOR);

    delete vocbase;
  }

  delete oldList;  // Note that this does not delete the TRI_vocbase_t pointers!
}

/// @brief create base app directory
int DatabaseFeature::createBaseApplicationDirectory(std::string const& appPath,
                                                    std::string const& type) {
  int res = TRI_ERROR_NO_ERROR;
  std::string path = arangodb::basics::FileUtils::buildFilename(appPath, type);
  
  if (!TRI_IsDirectory(path.c_str())) {
    std::string errorMessage;
    long systemError;
    res = TRI_CreateDirectory(path.c_str(), systemError, errorMessage);

    if (res == TRI_ERROR_NO_ERROR) {
      LOG(INFO) << "created base application directory '" << path << "'";
    } else {
      if ((res != TRI_ERROR_FILE_EXISTS) || (!TRI_IsDirectory(path.c_str()))) {
        LOG(ERR) << "unable to create base application directory "
                 << errorMessage;
      } else {
        LOG(INFO) << "someone else created base application directory '" << path
                  << "'";
        res = TRI_ERROR_NO_ERROR;
      }
    }
  }

  return res;
}

/// @brief create app subdirectory for a database
int DatabaseFeature::createApplicationDirectory(std::string const& name, std::string const& basePath) {
  if (basePath.empty()) {
    return TRI_ERROR_NO_ERROR;
  }

  std::string const path = basics::FileUtils::buildFilename(basics::FileUtils::buildFilename(basePath, "db"), name);
  int res = TRI_ERROR_NO_ERROR;

  if (!TRI_IsDirectory(path.c_str())) {
    long systemError;
    std::string errorMessage;
    res = TRI_CreateRecursiveDirectory(path.c_str(), systemError, errorMessage);

    if (res == TRI_ERROR_NO_ERROR) {
      if (arangodb::wal::LogfileManager::instance()->isInRecovery()) {
        LOG(TRACE) << "created application directory '" << path
                    << "' for database '" << name << "'";
      } else {
        LOG(INFO) << "created application directory '" << path
                  << "' for database '" << name << "'";
      }
    } else if (res == TRI_ERROR_FILE_EXISTS) {
      LOG(INFO) << "unable to create application directory '" << path
                << "' for database '" << name << "': " << errorMessage;
      res = TRI_ERROR_NO_ERROR;
    } else {
      LOG(ERR) << "unable to create application directory '" << path
                << "' for database '" << name << "': " << errorMessage;
    }
  }

  return res;
}

/// @brief iterate over all databases in the databases directory and open them
int DatabaseFeature::iterateDatabases(VPackSlice const& databases) {
  V8DealerFeature* dealer = ApplicationServer::getFeature<V8DealerFeature>("V8Dealer");
  std::string const appPath = dealer->appPath();
  std::string const databasePath = ApplicationServer::getFeature<DatabasePathFeature>("DatabasePath")->subdirectoryName("databases");
  
  StorageEngine* engine = ApplicationServer::getFeature<EngineSelectorFeature>("EngineSelector")->ENGINE;

  int res = TRI_ERROR_NO_ERROR;

  // open databases in defined order
  MUTEX_LOCKER(mutexLocker, _databasesMutex);

  auto oldLists = _databasesLists.load();
  auto newLists = new DatabasesLists(*oldLists);
  // No try catch here, if we crash here because out of memory...

  for (auto const& it : VPackArrayIterator(databases)) {
    TRI_ASSERT(it.isObject());

    std::string const databaseName = it.get("name").copyString();

    // create app directory for database if it does not exist
    res = createApplicationDirectory(databaseName, appPath);

    if (res != TRI_ERROR_NO_ERROR) {
      break;
    }

    // open the database and scan collections in it

    // try to open this database
    TRI_vocbase_t* vocbase = engine->openDatabase(it, _upgrade);
    // we found a valid database
    TRI_ASSERT(vocbase != nullptr);

    newLists->_databases.insert(std::make_pair(std::string(vocbase->_name), vocbase));
  }

  _databasesLists = newLists;
  _databasesProtector.scan();
  delete oldLists;
    
  return res;
}

/// @brief close all dropped databases
void DatabaseFeature::closeDroppedDatabases() {
  MUTEX_LOCKER(mutexLocker, _databasesMutex);

  // No need for the thread protector here, because we have the mutex
  // Note however, that somebody could still read the lists concurrently,
  // therefore we first install a new value, call scan() on the protector
  // and only then really destroy the vocbases:

  // Build the new value:
  auto oldList = _databasesLists.load();
  decltype(oldList) newList = nullptr;
  try {
    newList = new DatabasesLists();
    newList->_databases = _databasesLists.load()->_databases;
    newList->_coordinatorDatabases =
        _databasesLists.load()->_coordinatorDatabases;
  } catch (...) {
    delete newList;
    throw;
  }

  // Replace the old by the new:
  _databasesLists = newList;
  _databasesProtector.scan();

  // Now it is safe to destroy the old dropped databases and the old lists
  // struct:
  for (TRI_vocbase_t* vocbase : oldList->_droppedDatabases) {
    TRI_ASSERT(vocbase != nullptr);

    if (vocbase->_type == TRI_VOCBASE_TYPE_NORMAL) {
      TRI_DestroyVocBase(vocbase);
      delete vocbase;
    } else if (vocbase->_type == TRI_VOCBASE_TYPE_COORDINATOR) {
      delete vocbase;
    } else {
      LOG(ERR) << "unknown database type " << vocbase->_type << " "
               << vocbase->_name << " - close doing nothing.";
    }
  }

  delete oldList;  // Note that this does not delete the TRI_vocbase_t pointers!
}

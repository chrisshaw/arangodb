////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
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

#ifndef ARANGOD_REST_SERVER_ARANGO_SERVER_H
#define ARANGOD_REST_SERVER_ARANGO_SERVER_H 1

#include "Basics/Common.h"

#ifdef _WIN32
#include "Basics/win-utils.h"
#endif

#include "Aql/QueryRegistry.h"
#include "Rest/AnyServer.h"
#include "Rest/OperationMode.h"
#include "VocBase/vocbase.h"

struct TRI_server_t;
struct TRI_vocbase_defaults_t;


namespace triagens {
namespace basics {
class ThreadPool;
}

namespace rest {
class ApplicationDispatcher;
class ApplicationEndpointServer;
class ApplicationScheduler;
class AsyncJobManager;
class Dispatcher;
class HttpHandlerFactory;
class HttpServer;
class HttpsServer;
}

namespace arango {
class ApplicationV8;
class ApplicationCluster;


////////////////////////////////////////////////////////////////////////////////
/// @brief ArangoDB server
////////////////////////////////////////////////////////////////////////////////

class ArangoServer : public rest::AnyServer {
 private:
  ArangoServer(const ArangoServer&);
  ArangoServer& operator=(const ArangoServer&);

  
 public:

  ArangoServer(int, char**);


  ~ArangoServer();

  
 public:

  void buildApplicationServer();


  int startupServer();

  
 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief run arbitrary checks at startup
  //////////////////////////////////////////////////////////////////////////////

  void runStartupChecks();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief wait for the heartbeat thread to run
  /// before the server responds to requests, the heartbeat thread should have
  /// run at least once
  //////////////////////////////////////////////////////////////////////////////

  void waitForHeartbeat();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief runs in server mode
  //////////////////////////////////////////////////////////////////////////////

  int runServer(TRI_vocbase_t*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief runs in console mode
  //////////////////////////////////////////////////////////////////////////////

  int runConsole(TRI_vocbase_t*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief runs unit tests
  //////////////////////////////////////////////////////////////////////////////

  int runUnitTests(TRI_vocbase_t*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief runs script
  //////////////////////////////////////////////////////////////////////////////

  int runScript(TRI_vocbase_t*);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief opens all system databases
  //////////////////////////////////////////////////////////////////////////////

  void openDatabases(bool, bool, bool);

  //////////////////////////////////////////////////////////////////////////////
  /// @brief closes all database
  //////////////////////////////////////////////////////////////////////////////

  void closeDatabases();

  //////////////////////////////////////////////////////////////////////////////
  /// @brief defineHandlers, define "_api" and "_admin" handlers
  //////////////////////////////////////////////////////////////////////////////

  void defineHandlers(triagens::rest::HttpHandlerFactory* factory);

  
 private:
  //////////////////////////////////////////////////////////////////////////////
  /// @brief number of command line arguments
  //////////////////////////////////////////////////////////////////////////////

  int _argc;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief command line arguments
  //////////////////////////////////////////////////////////////////////////////

  char** _argv;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief temporary path
  //////////////////////////////////////////////////////////////////////////////

  std::string _tempPath;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief application scheduler
  //////////////////////////////////////////////////////////////////////////////

  rest::ApplicationScheduler* _applicationScheduler;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief application dispatcher
  //////////////////////////////////////////////////////////////////////////////

  rest::ApplicationDispatcher* _applicationDispatcher;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief application endpoint server
  //////////////////////////////////////////////////////////////////////////////

  rest::ApplicationEndpointServer* _applicationEndpointServer;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief cluster application feature
  //////////////////////////////////////////////////////////////////////////////

  triagens::arango::ApplicationCluster* _applicationCluster;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief asynchronous job manager
  //////////////////////////////////////////////////////////////////////////////

  rest::AsyncJobManager* _jobManager;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief application V8
  //////////////////////////////////////////////////////////////////////////////

  ApplicationV8* _applicationV8;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock serverAuthenticateSystemOnly
////////////////////////////////////////////////////////////////////////////////

  bool _authenticateSystemOnly;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock server_authentication
////////////////////////////////////////////////////////////////////////////////

  bool _disableAuthentication;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock serverAuthenticationDisable
////////////////////////////////////////////////////////////////////////////////

  bool _disableAuthenticationUnixSockets;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock serverThreads
////////////////////////////////////////////////////////////////////////////////

  int _dispatcherThreads;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief number of additional dispatcher threads
  //////////////////////////////////////////////////////////////////////////////

  std::vector<int> _additionalThreads;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock schedulerMaximalQueueSize
////////////////////////////////////////////////////////////////////////////////

  int _dispatcherQueueSize;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock v8Contexts
////////////////////////////////////////////////////////////////////////////////

  int _v8Contexts;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock indexThreads
////////////////////////////////////////////////////////////////////////////////

  int _indexThreads;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock DatabaseDirectory
////////////////////////////////////////////////////////////////////////////////

  std::string _databasePath;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock queryCacheMode
////////////////////////////////////////////////////////////////////////////////

  std::string _queryCacheMode;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock queryCacheMaxResults
////////////////////////////////////////////////////////////////////////////////

  uint64_t _queryCacheMaxResults;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock databaseMaximalJournalSize
////////////////////////////////////////////////////////////////////////////////

  TRI_voc_size_t _defaultMaximalSize;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock databaseWaitForSync
////////////////////////////////////////////////////////////////////////////////

  bool _defaultWaitForSync;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock databaseForceSyncProperties
////////////////////////////////////////////////////////////////////////////////

  bool _forceSyncProperties;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock databaseIgnoreDatafileErrors
////////////////////////////////////////////////////////////////////////////////

  bool _ignoreDatafileErrors;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock serverDisableReplicationApplier
////////////////////////////////////////////////////////////////////////////////

  bool _disableReplicationApplier;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock databaseDisableQueryTracking
////////////////////////////////////////////////////////////////////////////////

  bool _disableQueryTracking;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock databaseThrowCollectionNotLoadedError
////////////////////////////////////////////////////////////////////////////////

  bool _throwCollectionNotLoadedError;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock foxxQueues
////////////////////////////////////////////////////////////////////////////////

  bool _foxxQueues;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock foxxQueuesPollInterval
////////////////////////////////////////////////////////////////////////////////

  double _foxxQueuesPollInterval;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief unit tests
  ///
  /// @CMDOPT{\--javascript.unit-tests @CA{test-file}}
  ///
  /// Runs one or more unit tests.
  //////////////////////////////////////////////////////////////////////////////

  std::vector<std::string> _unitTests;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief files to jslint
  ///
  /// @CMDOPT{\--jslint @CA{test-file}}
  ///
  /// Runs jslint on one or more files.
  //////////////////////////////////////////////////////////////////////////////

  std::vector<std::string> _jslint;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief run script file
  ///
  /// @CMDOPT{\--javascript.script @CA{script-file}}
  ///
  /// Runs the script file.
  //////////////////////////////////////////////////////////////////////////////

  std::vector<std::string> _scriptFile;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief parameters to script file
  ///
  /// @CMDOPT{\--javascript.script-parameter @CA{script-parameter}}
  ///
  /// Parameter to script.
  //////////////////////////////////////////////////////////////////////////////

  std::vector<std::string> _scriptParameters;

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock DefaultLanguage
////////////////////////////////////////////////////////////////////////////////

  std::string _defaultLanguage;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the server
  //////////////////////////////////////////////////////////////////////////////

  TRI_server_t* _server;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief the server
  //////////////////////////////////////////////////////////////////////////////

  aql::QueryRegistry* _queryRegistry;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ptr to pair of _applicationV8 and _queryRegistry for _api/aql
  /// handler
  /// this will be removed once we have a global struct with "everything useful"
  //////////////////////////////////////////////////////////////////////////////

  std::pair<ApplicationV8*, aql::QueryRegistry*>* _pairForAqlHandler;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief ptr to pair used for job manager rest handler
  //////////////////////////////////////////////////////////////////////////////

  std::pair<triagens::rest::Dispatcher*, triagens::rest::AsyncJobManager*>*
      _pairForJobHandler;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief thread pool for background parallel index creation
  //////////////////////////////////////////////////////////////////////////////

  triagens::basics::ThreadPool* _indexPool;

  //////////////////////////////////////////////////////////////////////////////
  /// @brief use thread affinity
  //////////////////////////////////////////////////////////////////////////////

  uint32_t _threadAffinity;
};
}
}

#endif



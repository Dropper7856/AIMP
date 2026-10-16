cmake_minimum_required(VERSION 3.25)

project(AIMP.Native VERSION 0.11.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "AIMP.Native supports the MSVC x86 ABI only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure with -A Win32. AI.exe and AIMP.NetZ.dll are 32-bit.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(AIMP_CORE_SOURCES
    src/AISync.cpp
    src/AISyncHealth.cpp
    src/AISyncAnimation.cpp
    src/AISyncCombat.cpp
    src/AISyncNavigation.cpp
    src/BuildProfile.cpp
    src/CapabilityMatrix.cpp
    src/CaptureCatalog.cpp
    src/CathodeEquipmentGive.cpp
    src/CathodePrimitiveCodec.cpp
    src/CathodePrimitiveExecutor.cpp
    src/CharacterActionProbeDiagnostics.cpp
    src/CharacterActionRuntime.cpp
    src/ChannelInspector.cpp
    src/CharacterObservationHooks.cpp
    src/CharacterDirectStepRuntime.cpp
    src/CrashGuard.cpp
    src/DescriptorRegistry.cpp
    src/Diagnostics.cpp
    src/DummyPlayerController.cpp
    src/ExperimentalProtocol.cpp
    src/GameThreadDispatcher.cpp
    src/GameCommandExecutor.cpp
    src/GameThreadObservationHooks.cpp
    src/HistoricalParticipantTransaction.cpp
    src/HookManager.cpp
    src/InboundQueue.cpp
    src/LauncherIPC.cpp
    src/LevelGenerationTracker.cpp
    src/LifecycleCatalog.cpp
    src/NativeTransportAdapter.cpp
    src/NetZDiscovery.cpp
    src/ParticipantRegistry.cpp
    src/PlayerSyncObserver.cpp
    src/PlayerLifecycle.cpp
    src/RuntimeFeatureFlags.cpp
    src/Protocol.cpp
    src/ReadyReplayGate.cpp
    src/SignatureResolver.cpp
    src/SpawnCharacterTracker.cpp
    src/ReplicationCore.cpp
    src/WorldEntityRegistry.cpp
    src/WorldAnimatedAdapterCodec.cpp
    src/WorldAnimatedAdapters.cpp
    src/WorldInteractionAdapterCodec.cpp
    src/WorldInteractionAdapters.cpp
    src/WorldPublicActions.cpp
    src/WorldReplicationCodec.cpp
    src/WorldSemanticReconciliation.cpp
)

add_library(AIMP.Core STATIC ${AIMP_CORE_SOURCES})
set_target_properties(AIMP.Core PROPERTIES
    VS_GLOBAL_VcpkgEnabled "false"
    VS_GLOBAL_VcpkgApplocalDeps "false"
)
target_include_directories(AIMP.Core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(AIMP.Core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0602
)
target_compile_options(AIMP.Core PRIVATE
    /W4
    /WX
    /permissive-
    /EHsc
    /sdl
    /utf-8
    /Zc:__cplusplus
)
target_link_libraries(AIMP.Core PUBLIC bcrypt version)

add_library(AIMP.NetZ SHARED
    src/AIMP.NetZ.def
    src/Bootstrap.cpp
    src/DllMain.cpp
)
target_link_libraries(AIMP.NetZ PRIVATE AIMP.Core)
target_compile_options(AIMP.NetZ PRIVATE
    /W4
    /WX
    /permissive-
    /EHsc
    /sdl
    /utf-8
    /Zc:__cplusplus
)
set_target_properties(AIMP.NetZ PROPERTIES
    OUTPUT_NAME "AIMP.NetZ"
    PREFIX ""
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    VS_GLOBAL_VcpkgEnabled "false"
    VS_GLOBAL_VcpkgApplocalDeps "false"
)

include(CTest)
if(BUILD_TESTING)
    add_executable(AIMP.AISyncTests tests/AISyncPortableTests.cpp)
    set_target_properties(AIMP.AISyncTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false" VS_GLOBAL_VcpkgApplocalDeps "false")
    target_include_directories(AIMP.AISyncTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_options(AIMP.AISyncTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    add_test(NAME AISyncOwnershipAndNavigation COMMAND AIMP.AISyncTests)
    add_executable(AIMP.WorldActionTests EXCLUDE_FROM_ALL
        tests/WorldActionPortableTests.cpp
        src/WorldInteractionAdapterCodec.cpp)
    set_target_properties(AIMP.WorldActionTests PROPERTIES
        VS_GLOBAL_VcpkgEnabled "false"
        VS_GLOBAL_VcpkgApplocalDeps "false"
    )
    target_include_directories(AIMP.WorldActionTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(AIMP.WorldActionTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(AIMP.WorldActionTests PRIVATE /W4 /WX /permissive- /EHsc /utf-8)
    add_test(NAME WorldActionLineageAndWire COMMAND AIMP.WorldActionTests)
endif()

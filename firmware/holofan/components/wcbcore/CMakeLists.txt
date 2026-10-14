# The pure C++ libraries shared with the HoloLifter and the native tests
# compiled straight from the repo's lib/ directory.
set(LIB ${CMAKE_CURRENT_LIST_DIR}/../../../../lib)
file(GLOB WCB_SRCS ${LIB}/WcbCore/*.cpp)
idf_component_register(
  SRCS ${WCB_SRCS}
       ${LIB}/NodeConfig/NodeConfig.cpp
       ${LIB}/HolofanProtocol/command.cpp
       ${LIB}/FanLogic/McuProto.cpp
       ${LIB}/FanLogic/FanController.cpp
       ${LIB}/FanLogic/LoudTest.cpp
       ${LIB}/FanLogic/SagLog.cpp
       ${LIB}/FanLogic/ClipOps.cpp
       ${LIB}/FanLogic/FrameRing.cpp
       ${LIB}/FanLogic/ChunkFeed.cpp
       ${LIB}/FanLogic/UploadSession.cpp
       ${LIB}/LiftCommands/LiftCommands.cpp
       ${LIB}/MaintCore/MaintFsm.cpp
       ${LIB}/MaintCore/ValidityGate.cpp
       ${LIB}/MaintCore/PowerCycleTrigger.cpp
  INCLUDE_DIRS ${LIB}/WcbCore ${LIB}/NodeConfig ${LIB}/HolofanProtocol ${LIB}/FanLogic ${LIB}/LiftCommands ${LIB}/MaintCore)

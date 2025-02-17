lldb_tablegen(Options.inc -gen-lldb-option-defs
  SOURCE Options.td
  TARGET LLDBOptionsGen)

add_lldb_library(lldbCommands
  CommandCompletions.cpp
  CommandObjectApropos.cpp
  CommandObjectBreakpoint.cpp
  CommandObjectBreakpointCommand.cpp
  CommandObjectBugreport.cpp
  CommandObjectCommands.cpp
  CommandObjectDisassemble.cpp
  CommandObjectExpression.cpp
  CommandObjectFrame.cpp
  CommandObjectGUI.cpp
  CommandObjectHelp.cpp
  CommandObjectLog.cpp
  CommandObjectMemory.cpp
  CommandObjectMultiword.cpp
  CommandObjectPlatform.cpp
  CommandObjectPlugin.cpp
  CommandObjectProcess.cpp
  CommandObjectQuit.cpp
  CommandObjectRegister.cpp
  CommandObjectReproducer.cpp
  CommandObjectSettings.cpp
  CommandObjectSource.cpp
  CommandObjectStats.cpp
  CommandObjectTarget.cpp
  CommandObjectThread.cpp
  CommandObjectType.cpp
  CommandObjectVersion.cpp
  CommandObjectWatchpoint.cpp
  CommandObjectWatchpointCommand.cpp
  CommandObjectLanguage.cpp

  LINK_LIBS
    lldbBase
    lldbBreakpoint
    lldbCore
    lldbDataFormatters
    lldbExpression
    lldbHost
    lldbInterpreter
    lldbSymbol
    lldbTarget
    lldbUtility

  LINK_COMPONENTS
    Support
  )

add_dependencies(lldbCommands LLDBOptionsGen)

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#define MANIFOLD_ERROR_CODES(X) \
    X(ProcessNotFound, 1001) \
    X(ProcessAccessDenied, 1002) \
    X(ProcessOpenFailed, 1003) \
    X(ProcessExited, 1004) \
    X(NoProcess, 1005) \
    X(ReadFailed, 1006) \
    X(WriteFailed, 1007) \
    X(AllocFailed, 1008) \
    X(ProtectFailed, 1009) \
    X(FreeFailed, 1010) \
    X(ModulesUnreadable, 1011) \
    X(ThreadCreateFailed, 1012) \
    X(InjectFailed, 1013) \
    X(ModuleFileUnreadable, 1014) \
    X(ModuleFileDifferent, 1015) \
    X(ScanNoValue, 2001) \
    X(ScanBadValue, 2002) \
    X(ScanBadPattern, 2003) \
    X(ScanStoreFailed, 2004) \
    X(ScanRangeEmpty, 2005) \
    X(ScanBusy, 2006) \
    X(ScanNothingToCompare, 2007) \
    X(ScanNoTypes, 2008) \
    X(ScanGroupSyntax, 2009) \
    X(ScanCompareUnavailable, 2010) \
    X(ScanSavedMissing, 2011) \
    X(ScanSavedDifferent, 2012) \
    X(ScanCustomMissing, 2013) \
    X(AsmSyntax, 3001) \
    X(AsmUnknownInstruction, 3002) \
    X(AsmBadOperand, 3003) \
    X(AsmUnknownSymbol, 3004) \
    X(AsmOutOfRange, 3005) \
    X(AsmEncodeFailed, 3006) \
    X(AaUnknownCommand, 3101) \
    X(AaBadArguments, 3102) \
    X(AaLabelTwice, 3103) \
    X(AaLabelNotPlaced, 3104) \
    X(AaAllocFailed, 3105) \
    X(AaPatternNotFound, 3106) \
    X(AaAssertFailed, 3107) \
    X(AaNoSections, 3108) \
    X(AaWriteFailed, 3109) \
    X(AaLuaFailed, 3110) \
    X(AaLayoutUnstable, 3111) \
    X(AaIncludeMissing, 3112) \
    X(AaSymbolTaken, 3113) \
    X(AaReadMemFailed, 3114) \
    X(DebugAttachFailed, 4001) \
    X(DebugAlreadyAttached, 4002) \
    X(DebugNoFreeSlot, 4003) \
    X(DebugBreakpointFailed, 4004) \
    X(DebugContextFailed, 4005) \
    X(DebugDetachFailed, 4006) \
    X(DebugNotAttached, 4007) \
    X(DebugBadSize, 4008) \
    X(DebugTargetExited, 4009) \
    X(DebugBadExpression, 4010) \
    X(DebugAgentVersion, 4011) \
    X(DebugTraceFailed, 4012) \
    X(MonoNotFound, 4101) \
    X(MonoCallFailed, 4102) \
    X(WindowClassFailed, 5001) \
    X(WindowFailed, 5002) \
    X(DeviceFailed, 5003) \
    X(FactoryFailed, 5004) \
    X(SwapChainFailed, 5005) \
    X(IconFontMissing, 5006) \
    X(FontAtlasFailed, 5007) \
    X(ImGuiFailed, 5008) \
    X(SettingsNotSaved, 5009) \
    X(Crashed, 5010) \
    X(LastRunCrashed, 5011) \
    X(NotElevated, 5012) \
    X(TableNotRead, 6001) \
    X(TableNotXml, 6002) \
    X(TableNotCheatTable, 6003) \
    X(TableNotSaved, 6004) \
    X(TableEntryDamaged, 6005) \
    X(TableTooNew, 6006) \
    X(LuaSyntax, 7001) \
    X(LuaRuntime, 7002) \
    X(LuaAutorunFailed, 7003) \
    X(LuaTimerFailed, 7004) \
    X(LuaTableScriptFailed, 7005) \
    X(LuaBadArgument, 7006) \
    X(ExprSyntax, 8001) \
    X(ExprUnknownSymbol, 8002) \
    X(ExprPointerUnreadable, 8003) \
    X(ExprDivideByZero, 8004) \
    X(SymbolNameTaken, 8005) \
    X(TemplateSyntax, 9001) \
    X(TemplateRender, 9002) \
    X(TemplateRequirement, 9003) \
    X(TemplateContext, 9004) \
    X(TemplateNotFound, 9005) \
    X(TemplateIncludeCycle, 9006) \
    X(TemplateArchitecture, 9007) \
    X(TemplateWrite, 9008)

namespace Manifold
{
    enum class ErrorCode : std::uint16_t
    {
        None = 0,
#define MANIFOLD_ERROR_ENUM(name, number) name = number,
        MANIFOLD_ERROR_CODES(MANIFOLD_ERROR_ENUM)
#undef MANIFOLD_ERROR_ENUM
    };

    inline constexpr ErrorCode kErrorCodes[] = {
#define MANIFOLD_ERROR_LIST(name, number) ErrorCode::name,
        MANIFOLD_ERROR_CODES(MANIFOLD_ERROR_LIST)
#undef MANIFOLD_ERROR_LIST
    };

    struct ErrorInfo
    {
        ErrorCode Code;
        std::string_view Message;
        std::string_view Cause;
        std::string_view Help;
    };

    std::span<const ErrorInfo> ErrorTable();
    const ErrorInfo* FindError(ErrorCode code);
    const ErrorInfo* FindError(std::string_view written);
    const ErrorInfo* ErrorOf(std::string_view message);

    std::string ErrorName(ErrorCode code);
    std::string_view ErrorArea(ErrorCode code);
    std::string ErrorMessage(ErrorCode code, std::string_view detail = {});
    std::string ErrorsMarkdown();

    struct Status
    {
        ErrorCode Code = ErrorCode::None;
        std::string Text;

        bool Ok() const { return Code == ErrorCode::None; }
        explicit operator bool() const { return Ok(); }

        static Status Success() { return {}; }
        static Status Fail(ErrorCode code, std::string_view detail = {}) { return { code, ErrorMessage(code, detail) }; }
    };
}

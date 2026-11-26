#include "Lua/LuaCompatScripts.h"

#include <array>

namespace Manifold::Lua
{
    namespace
    {
        constexpr std::array<std::string_view, 1> kScript0 = {
            std::string_view(R"MLUA(local defines = {
  cbUnchecked = 0,
  cbChecked = 1,
  cbGrayed = 2,
  mbLeft = 0,
  mbRight = 1,
  mbMiddle = 2,
  mbExtra1 = 3,
  mbExtra2 = 4,
  ssNone = 0,
  ssHorizontal = 1,
  ssVertical = 2,
  ssBoth = 3,
  ssAutoHorizontal = 4,
  ssAutoVertical = 5,
  ssAutoBoth = 6,
  bsNone = 0,
  bsSingle = 1,
  bsSizeable = 2,
  bsDialog = 3,
  bsToolWindow = 4,
  bsSizeToolWin = 5,
  fsmNotAligned = 0,
  fsmAligned = 1,
  fsmLastDigits = 2,
  rtRounded = 0,
  rtExtremerounded = 1,
  rtTruncated = 2,
  soUnknownValue = 0,
  soExactValue = 1,
  soValueBetween = 2,
  soBiggerThan = 3,
  soSmallerThan = 4,
  soIncreasedValue = 5,
  soIncreasedValueBy = 6,
  soDecreasedValue = 7,
  soDecreasedValueBy = 8,
  soChanged = 9,
  soUnchanged = 10,
  bpmInt3 = 0,
  bpmDebugRegister = 1,
  bpmException = 2,
  bptExecute = 0,
  bptAccess = 1,
  bptWrite = 2,
  co_run = 0,
  co_stepinto = 1,
  co_stepover = 2,
  caNone = 0,
  caHide = 1,
  caFree = 2,
  caMinimize = 3,
  alNone = 0,
  alTop = 1,
  alBottom = 2,
  alLeft = 3,
  alRight = 4,
  alClient = 5,
  mtWarning = 0,
  mtError = 1,
  mtInformation = 2,
  mtConfirmation = 3,
  mbYes = 0,
  mbNo = 1,
  mbOK = 2,
  mbCancel = 3,
  mbAbort = 4,
  mbRetry = 5,
  mbIgnore = 6,
  mbAll = 7,
  mbNoToAll = 8,
  mbYesToAll = 9,
  mbHelp = 10,
  mbClose = 11,
  mrNone = 0,
  mrOK = 1,
  mrCancel = 2,
  mrAbort = 3,
  mrRetry = 4,
  mrIgnore = 5,
  mrYes = 6,
  mrNo = 7,
  mrAll = 8,
  mrNoToAll = 9,
  mrYesToAll = 10,
  mrLast = 10,
  dupIgnore = 0,
  dupAccept = 1,
  dupError = 2,
  vtByte = 0,
  vtWord = 1,
  vtDword = 2,
  vtQword = 3,
  vtSingle = 4,
  vtDouble = 5,
  vtString = 6,
  vtUnicodeString = 7,
  vtWideString = 7,
  vtByteArray = 8,
  vtBinary = 9,
  vtAll = 10,
  vtAutoAssembler = 11,
  vtPointer = 12,
  vtCustom = 13,
  vtGrouped = 14,
  VK_LBUTTON = 1,
  VK_RBUTTON = 2,
  VK_CANCEL = 3,
  VK_MBUTTON = 4,
  VK_XBUTTON1 = 5,
  VK_XBUTTON2 = 6,
  VK_BACK = 8,
  VK_TAB = 9,
  VK_CLEAR = 12,
  VK_RETURN = 13,
  VK_SHIFT = 16,
  VK_CONTROL = 17,
  VK_MENU = 18,
  VK_PAUSE = 19,
  VK_CAPITAL = 20,
  VK_ESCAPE = 27,
  VK_SPACE = 32,
  VK_PRIOR = 33,
  VK_NEXT = 34,
  VK_END = 35,
  VK_HOME = 36,
  VK_LEFT = 37,
  VK_UP = 38,
  VK_RIGHT = 39,
  VK_DOWN = 40,
  VK_SELECT = 41,
  VK_PRINT = 42,
  VK_EXECUTE = 43,
  VK_SNAPSHOT = 44,
  VK_INSERT = 45,
  VK_DELETE = 46,
  VK_HELP = 47,
  VK_0 = 48,
  VK_1 = 49,
  VK_2 = 50,
  VK_3 = 51,
  VK_4 = 52,
  VK_5 = 53,
  VK_6 = 54,
  VK_7 = 55,
  VK_8 = 56,
  VK_9 = 57,
  VK_A = 65,
  VK_B = 66,
  VK_C = 67,
  VK_D = 68,
  VK_E = 69,
  VK_F = 70,
  VK_G = 71,
  VK_H = 72,
  VK_I = 73,
  VK_J = 74,
  VK_K = 75,
  VK_L = 76,
  VK_M = 77,
  VK_N = 78,
  VK_O = 79,
  VK_P = 80,
  VK_Q = 81,
  VK_R = 82,
  VK_S = 83,
  VK_T = 84,
  VK_U = 85,
  VK_V = 86,
  VK_W = 87,
  VK_X = 88,
  VK_Y = 89,
  VK_Z = 90,
  VK_LWIN = 91,
  VK_RWIN = 92,
  VK_APPS = 93,
  VK_NUMPAD0 = 96,
  VK_NUMPAD1 = 97,
  VK_NUMPAD2 = 98,
  VK_NUMPAD3 = 99,
  VK_NUMPAD4 = 100,
  VK_NUMPAD5 = 101,
  VK_NUMPAD6 = 102,
  VK_NUMPAD7 = 103,
  VK_NUMPAD8 = 104,
  VK_NUMPAD9 = 105,
  VK_MULTIPLY = 106,
  VK_ADD = 107,
  VK_SEPARATOR = 108,
  VK_SUBTRACT = 109,
  VK_DECIMAL = 110,
  VK_DIVIDE = 111,
  VK_F1 = 112,
  VK_F2 = 113,
  VK_F3 = 114,
  VK_F4 = 115,
  VK_F5 = 116,
  VK_F6 = 117,
  VK_F7 = 118,
  VK_F8 = 119,
  VK_F9 = 120,
  VK_F10 = 121,
  VK_F11 = 122,
  VK_F12 = 123,
  VK_F13 = 124,
  VK_F14 = 125,
  VK_F15 = 126,
  VK_F16 = 127,
  VK_F17 = 128,
  VK_F18 = 129,
  VK_F19 = 130,
  VK_F20 = 131,
  VK_F21 = 132,
  VK_F22 = 133,
  VK_F23 = 134,
  VK_F24 = 135,
  VK_NUMLOCK = 144,
  VK_SCROLL = 145,
  VK_LSHIFT = 160,
  VK_LCONTROL = 162,
  VK_LMENU = 164,
  VK_RSHIFT = 161,
  VK_RCONTROL = 163,
  VK_RMENU = 165,
  VK_OEM1 = 186,
  VK_OEM2 = 191,
  VK_OEM3 = 192,
  SW_HIDE = 0,
  SW_MAXIMIZE = 3,
  SW_MINIMIZE = 6,
  SW_NORMAL = 1,
  SW_RESTORE = 9,
  SW_SHOW = 5,
  SW_SHOWDEFAULT = 10,
  SW_SHOWMAXIMIZED = 3,
  SW_SHOWMINIMIZED = 2,
  SW_SHOWMINNOACTIVE = 7,
  SW_SHOWNA = 8,
  SW_SHOWNOACTIVATE = 4,
  SW_SHOWNORMAL = 1,
  pfDevice = 0,
  pf1bit = 1,
  pf4bit = 2,
  pf8bit = 3,
  pf15bit = 4,
  pf16bit = 5,
  pf24bit = 6,
  pf32bit = 7,
  pfCustom = 8,
  dvtNone = 0,
  dvtAddress = 1,
  dvtValue = 2,
  jtCall = 0,
  jtUnconditional = 1,
  jtConditional = 2,
  jtMemory = 3,
  slStart = 0,
  slNotInt = 1,
  slNotModule = 2,
  slNotUserdefinedSymbol = 3,
  slNotSymbol = 4,
  slFailure = 5,
  poDesigned = 0,
  poDefault = 1,
  poDefaultPosOnly = 2,
  poDefaultSizeOnly = 3,
  poScreenCenter = 4,
  poDesktopCenter = 5,
  poMainFormCenter = 6,
  poOwnerFormCenter = 7,
  asrTop = 0,
  asrBottom = 1,
  asrCenter = 2,
  asrLeft = 0,
  asrRight = 1,
  vsIcon = 0,
  vsSmallIcon = 1,
  vsList = 2,
  vsReport = 3,
  LWA_COLORKEY = 1,
  LWA_ALPHA = 2,
  GW_HWNDFIRST = 0,
  GW_HWNDLAST = 1,
  GW_HWNDNEXT = 2,
  GW_HWNDPREV = 3,
  GW_HWNDOWNER = 4,
  GW_OWNER = 4,
  GW_CHILD = 5,
  GW_ENABLEDPOPUP = 6,
  mrhToggleActivation = 0,
  mrhToggleActivationAllowIncrease = 1,
  mrhToggleActivationAllowDecrease = 2,
  mrhActivate = 3,
  mrhDeactivate = 4,
  mrhSetValue = 5,
  mrhIncreaseValue = 6,
  mrhDecreaseValue = 7,
  MOUSEEVENTF_MOVE = 1,
  MOUSEEVENTF_LEFTDOWN = 2,
  MOUSEEVENTF_LEFTUP = 4,
  MOUSEEVENTF_RIGHTDOWN = 8,
  MOUSEEVENTF_RIGHTUP = 16,
  MOUSEEVENTF_MIDDLEDOWN = 32,
  MOUSEEVENTF_MIDDLEUP = 64,
  MOUSEEVENTF_XDOWN = 128,
  MOUSEEVENTF_XUP = 256,
  MOUSEEVENTF_WHEEL = 2048,
  MOUSEEVENTF_HWHEEL = 4096,
  MOUSEEVENTF_ABSOLUTE = 32768,
  SPF_DEFAULT = 0,
  SPF_ASYNC = 1,
  SPF_PURGEBEFORESPEAK = 2,
  SPF_IS_FILENAME = 4,
  SPF_IS_XML = 8,
  SPF_IS_NOT_XML = 16,
  SPF_PERSIST_XML = 32,
  SPF_NLP_SPEAK_PUNC = 64,
  SPF_PARSE_SAPI = 128,
  SPF_PARSE_SSML = 256,
  SPF_PARSE_AUTODETECT = 0,
  wrSignaled = 0,
  wrTimeout = 1,
  wrAbandoned = 2,
  wrError = 3,
  fsSurface = 0,
  fsBorder = 1,
  cdsSelected = 0,
  cdsGrayed = 1,
  cdsDisabled = 2,
  cdsChecked = 3,
  cdsFocused = 4,
  cdsDefault = 5,
  cdsHot = 6,
  cdsMarked = 7,
  cdsIndeterminate = 8,
  cdPrePaint = 0,
  cdPostPaint = 1,
  cdPreErase = 2,
  cdPostErase = 3,
  drBounds = 0,
  drIcon = 1,
  drLabel = 2,
  drSelectBounds = 3,
  fmCreate = 65280,
  fmOpenRead = 0,
  fmOpenWrite = 1,
  fmOpenReadWrite = 2,
  fmShareCompat = 0,
  fmShareExclusive = 16,
  fmShareDenyWrite = 32,
  fmShareDenyRead = 48,
  fmShareDenyNone = 64,
  crDefault = 0,
  crNone = -1,
  crArrow = -2,
  crCross = -3,
  crIBeam = -4,
  crSize = -22,
  crSizeNESW = -6,
  crSizeNS = -7,
  crSizeNWSE = -8,
  crSizeWE = -9,
  crSizeNW = -23,
  crSizeN = -24,
  crSizeNE = -25,
  crSizeW = -26,
  crSizeE = -27,
  crSizeSW = -28,
  crSizeS = -29,
  crSizeSE = -30,
  crUpArrow = -10,
  crHourGlass = -11,
  crDrag = -12,
  crNoDrop = -13,
  crHSplit = -14,
  crVSplit = -15,
  crMultiDrag = -16,
  crSQLWait = -17,
  crNo = -18,
  crAppStart = -19,
  crHelp = -20,
  crHandPoint = -21,
  crSizeAll = -22,
  CLR_NONE = 4294967295,
  CLR_DEFAULT = 4278190080,
  CLR_INVALID = 4294967295,
  COLOR_SCROLLBAR = 0,
  COLOR_BACKGROUND = 1,
  COLOR_ACTIVECAPTION = 2,
  COLOR_INACTIVECAPTION = 3,
  COLOR_MENU = 4,
  COLOR_WINDOW = 5,
  COLOR_WINDOWFRAME = 6,
  COLOR_MENUTEXT = 7,
  COLOR_WINDOWTEXT = 8,
  COLOR_CAPTIONTEXT = 9,
  COLOR_ACTIVEBORDER = 10,
  COLOR_INACTIVEBORDER = 11,
  COLOR_APPWORKSPACE = 12,
  COLOR_HIGHLIGHT = 13,
  COLOR_HIGHLIGHTTEXT = 14,
  COLOR_BTNFACE = 15,
  COLOR_BTNSHADOW = 16,
  COLOR_GRAYTEXT = 17,
  COLOR_BTNTEXT = 18,
  COLOR_INACTIVECAPTIONTEXT = 19,
  COLOR_BTNHIGHLIGHT = 20,
  COLOR_3DDKSHADOW = 21,
  COLOR_3DLIGHT = 22,
  COLOR_INFOTEXT = 23,
  COLOR_INFOBK = 24,
  COLOR_HOTLIGHT = 26,
  COLOR_GRADIENTACTIVECAPTION = 27,
  COLOR_GRADIENTINACTIVECAPTION = 28,
  COLOR_MENUHILIGHT = 29,
  COLOR_MENUBAR = 30,
  COLOR_FORM = 31,
  COLOR_ENDCOLORS = 31,
  COLOR_DESKTOP = 1,
  COLOR_3DFACE = 15,
  COLOR_3DSHADOW = 16,
  COLOR_3DHIGHLIGHT = 20,
  COLOR_3DHILIGHT = 20,
  COLOR_BTNHILIGHT = 20,
  MAX_SYS_COLORS = 31,
  SYS_COLOR_BASE = 2147483648,
  clBlack = 0,
  clMaroon = 128,
  clGreen = 32768,
  clOlive = 32896,
  clNavy = 8388608,
  clPurple = 8388736,
  clTeal = 8421376,
  clGray = 8421504,
  clSilver = 12632256,
  clOrange = 33023,
  clRed = 255,
  clLime = 65280,
  clYellow = 65535,
  clBlue = 16711680,
  clFuchsia = 16711935,
  clAqua = 16776960,
  clLtGray = 12632256,
  clDkGray = 8421504,
  clWhite = 16777215,
  StandardColorsCount = 16,
  clMoneyGreen = 12639424,
  clSkyBlue = 15780518,
  clCream = 15793151,
  clMedGray = 10789024,
  ExtendedColorCount = 4,
  clNone = 536870911,
  clDefault = 536870912,
  clScrollBar = 2147483648,
  clBackground = 2147483649,
  clActiveCaption = 2147483650,
  clInactiveCaption = 2147483651,
  clMenu = 2147483652,
  clWindow = 2147483653,
  clWindowFrame = 2147483654,
  clMenuText = 2147483655,
  clWindowText = 2147483656,
  clCaptionText = 2147483657,
  clActiveBorder = 2147483658,
  clInactiveBorder = 2147483659,
  clAppWorkspace = 2147483660,
  clHighlight = 2147483661,
  clHighlightText = 2147483662,
  clBtnFace = 2147483663,
  clBtnShadow = 2147483664,
  clGrayText = 2147483665,
  clBtnText = 2147483666,
  clInactiveCaptionText = 2147483667,
  clBtnHighlight = 2147483668,
  cl3DDkShadow = 2147483669,
  cl3DLight = 2147483670,
  clInfoText = 2147483671,
  clInfoBk = 2147483672,
  clHotLight = 2147483674,
  clGradientActiveCaption = 2147483675,
  clGradientInactiveCaption = 2147483676,
  clMenuHighlight = 2147483677,
  clMenuBar = 2147483678,
  clForm = 2147483679,
  clColorDesktop = 2147483649,
  cl3DFace = 2147483663,
  cl3DShadow = 2147483664,
  cl3DHiLight = 2147483668,
  clBtnHiLight = 2147483668,
  clFirstSpecialColor = 2147483668,
  clMask = 16777215,
  clDontMask = 0,
  pmBlack = 0,
  pmWhite = 1,
  pmNop = 2,
  pmNot = 3,
  pmCopy = 4,
  pmNotCopy = 5,
  pmMergePenNot = 6,
  pmMaskPenNot = 7,
  pmMergeNotPen = 8,
  pmMaskNotPen = 9,
  pmMerge = 10,
  pmNotMerge = 11,
  pmMask = 12,
  pmNotMask = 13,
  pmXor = 14,
  pmNotXor = 15,
  tbpsNone = 0,
  tbpsIndeterminate = 1,
  tbpsNormal = 2,
  tbpsError = 3,
  tbpsPaused = 4,
  PAGE_READONLY = 2,
  PAGE_READWRITE = 4,
  PAGE_WRITECOPY = 8,
  PAGE_EXECUTE = 16,
  PAGE_EXECUTE_READ = 32,
  PAGE_EXECUTE_READWRITE = 64,
  PAGE_EXECUTE_WRITECOPY = 128,
  DT_UNKNOWN = 0,
  DT_FIFO = 1,
  DT_CHR = 2,
  DT_DIR = 4,
  DT_BLK = 6,
  DT_REG = 8,
  DT_LNK = 10,
  DT_SOCK = 12,
  DT_WHT = 14,
}
for name, value in pairs(defines) do
  if rawget(_G, name) == nil then
    rawset(_G, name, value)
  end
end
)MLUA"),
        };
        constexpr std::array<std::string_view, 5> kScript1 = {
            std::string_view(R"MLUA(__ceClasses = {
  ["addresslist"] = {
    parent = "panel",
    props = {["checkboxactivecolor"] = "color", ["checkboxactiveselectedcolor"] = "color", ["checkboxcolor"] = "color", ["checkboxselectedcolor"] = "color", ["count"] = "integer", ["decreasearrowcolor"] = "color", ["expandsigncolor"] = "color", ["increasearrowcolor"] = "color", ["loadedtableversion"] = "integer", ["onaddresschange"] = "function", ["onautoassembleredit"] = "function", ["ondescriptionchange"] = "function", ["ontypechange"] = "function", ["onvaluechange"] = "function", ["selcount"] = "integer", ["selectedbackgroundcolor"] = "color", ["selectedrecord"] = "memoryrecord", ["selectedsecondarybackgroundcolor"] = "color"},
    indexed = {["memoryrecord"] = "memoryrecord"},
    methods = {["creatememoryrecord"] = true, ["disableallwithoutexecute"] = true, ["doaddresschange"] = true, ["dodescriptionchange"] = true, ["dotypechange"] = true, ["dovaluechange"] = true, ["getcount"] = true, ["getmemoryrecord"] = true, ["getmemoryrecordbydescription"] = true, ["getmemoryrecordbyid"] = true, ["getselectedrecord"] = true, ["getselectedrecords"] = true, ["mousehighlightedrecord"] = true, ["rebuilddescriptioncache"] = true, ["setselectedrecord"] = true},
  },
  ["application"] = {
    parent = "customapplication",
    props = {["icon"] = "the", ["title"] = "the"},
    indexed = {},
    methods = {["bringtofront"] = true, ["minimize"] = true, ["processmessages"] = true, ["terminate"] = true},
  },
  ["bitmap"] = {
    parent = "custombitmap",
    props = {},
    indexed = {},
    methods = {},
  },
  ["brush"] = {
    parent = "custombrush",
    props = {["color"] = "integer"},
    indexed = {},
    methods = {["getcolor"] = true, ["setcolor"] = true},
  },
  ["button"] = {
    parent = "buttoncontrol",
    props = {["modalresult"] = "modalresult"},
    indexed = {},
    methods = {["getmodalresult"] = true, ["setmodalresult"] = true},
  },
  ["buttoncontrol"] = {
    parent = "wincontrol",
    props = {},
    indexed = {},
    methods = {},
  },
  ["calendar"] = {
    parent = "wincontrol",
    props = {["date"] = "string", ["datetime"] = "number"},
    indexed = {},
    methods = {},
  },
  ["canvas"] = {
    parent = "customcanvas",
    props = {["brush"] = "brush", ["font"] = "font", ["fsborder"] = "fill", ["fssurface"] = "fill", ["handle"] = "integer", ["height"] = "integer", ["pen"] = "pen", ["width"] = "integer"},
    indexed = {},
    methods = {["clear"] = true, ["copyrect"] = true, ["draw"] = true, ["drawfocusrect"] = true, ["ellipse"] = true, ["fillrect"] = true, ["floodfill"] = true, ["getbrush"] = true, ["getcliprect"] = true, ["getfont"] = true, ["getheight"] = true, ["getpen"] = true, ["getpenposition"] = true, ["getpixel"] = true, ["gettextheight"] = true, ["gettextwidth"] = true, ["getwidth"] = true, ["gradientfill"] = true, ["line"] = true, ["lineto"] = true, ["moveto"] = true, ["rect"] = true, ["roundrect"] = true, ["setpenposition"] = true, ["setpixel"] = true, ["stretchdraw"] = true, ["textout"] = true, ["textrect"] = true},
  },
  ["cecustombutton"] = {
    parent = "customcontrol",
    props = {["bordercolor"] = "color", ["bordersize"] = "integer", ["buttonanimationspeed"] = "integer", ["buttoncolor"] = "color", ["buttondowncolor"] = "color", ["buttonhighlightedcolor"] = "color", ["customdrawn"] = "boolean", ["drawborder"] = "boolean", ["drawfocusrect"] = "boolean", ["focusedsize"] = "integer", ["focuselipsecolor"] = "color", ["framespersecond"] = "integer", ["growfont"] = "boolean", ["roundingx"] = "integer", ["roundingy"] = "integer", ["showprefix"] = "boolean"},
    indexed = {},
    methods = {["startanimatortimer"] = true, ["stopanimatortimer"] = true},
  },
  ["ceform"] = {
    parent = "form",
    props = {["donotsaveintable"] = "boolean"},
    indexed = {},
    methods = {["getdonotsaveintable"] = true, ["savecurrentstateasdesign"] = true, ["savetofile"] = true, ["savetostream"] = true, ["setdonotsaveintable"] = true},
  },
  ["cheatcomponent"] = {
    parent = "wincontrol",
    props = {["activated"] = "boolean", ["activationcolor"] = "integer", ["color"] = "integer", ["description"] = "string", ["descriptionleft"] = "integer", ["editleft"] = "integer", ["editvalue"] = "string", ["editwidth"] = "integer", ["font"] = "font", ["hascheckbox"] = "boolean", ["haseditbox"] = "boolean", ["hotkey"] = "string", ["hotkeyleft"] = "integer", ["showhotkey"] = "boolean", ["textcolor"] = "integer"},
    indexed = {},
    methods = {},
  },
  ["checkbox"] = {
    parent = "buttoncontrol",
    props = {["allowgrayed"] = "boolean", ["checked"] = "boolean", ["onchange"] = "function", ["state"] = "checkboxstate"},
    indexed = {},
    methods = {["getallowgrayed"] = true, ["getstate"] = true, ["onchange"] = true, ["setallowgrayed"] = true, ["setstate"] = true},
  },
  ["collection"] = {
    parent = "object",
    props = {["count"] = "integer"},
    indexed = {["items"] = "collectionitem"},
    methods = {["clear"] = true, ["delete"] = true, ["getcount"] = true},
  },
  ["collectionitem"] = {
    parent = "object",
    props = {["displayname"] = "string", ["id"] = "integer", ["index"] = "integer"},
    indexed = {},
    methods = {["getdisplayname"] = true, ["getid"] = true, ["getindex"] = true, ["setdisplayname"] = true, ["setindex"] = true},
  },
  ["combobox"] = {
    parent = "wincontrol",
    props = {["canvas"] = "canvas", ["itemindex"] = "integer", ["items"] = "strings"},
    indexed = {},
    methods = {["clear"] = true, ["getcanvas"] = true, ["getextrawidth"] = true, ["getitemindex"] = true, ["getitems"] = true, ["setitemindex"] = true, ["setitems"] = true},
  },
  ["commondialog"] = {
    parent = "component",
    props = {["onclose"] = "function", ["onshow"] = "function", ["title"] = "string"},
    indexed = {},
    methods = {["execute"] = true},
  },
  ["component"] = {
    parent = "object",
    props = {["componentcount"] = "integer", ["name"] = "string", ["owner"] = "component", ["tag"] = "integer"},
    indexed = {["component"] = "component", ["componentbyname"] = "component"},
    methods = {["findcomponentbyname"] = true, ["getcomponent"] = true, ["getcomponentcount"] = true, ["getname"] = true, ["getowner"] = true, ["gettag"] = true, ["setname"] = true, ["settag"] = true},
  },
  ["control"] = {
    parent = "component",
    props = {["align"] = "alignmentoption", ["caption"] = "string", ["clientheight"] = "integer", ["clientwidth"] = "integer", ["color"] = "colordefinition", ["enabled"] = "boolean", ["font"] = "font", ["height"] = "integer", ["left"] = "integer", ["onchangebounds"] = "function", ["onclick"] = "function", ["parent"] = "wincontrol", ["popupmenu"] = "popupmenu", ["rgbcolor"] = "rgbinteger", ["top"] = "integer", ["visible"] = "boolean", ["width"] = "integer"},
    indexed = {},
    methods = {["bringtofront"] = true, ["clienttoscreen"] = true, ["doclick"] = true, ["getalign"] = true, ["getcaption"] = true, ["getcolor"] = true, ["getenabled"] = true, ["getfont"] = true, ["getheight"] = true, ["getleft"] = true, ["getonclick"] = true, ["getparent"] = true, ["getpopupmenu"] = true, ["getposition"] = true, ["getsize"] = true, ["gettop"] = true, ["getvisible"] = true, ["getwidth"] = true, ["refresh"] = true, ["repaint"] = true, ["screentoclient"] = true, ["sendtoback"] = true, ["setalign"] = true, ["setcaption"] = true, ["setcolor"] = true, ["setenabled"] = true, ["setfont"] = true, ["setheight"] = true, ["setleft"] = true, ["setonclick"] = true, ["setparent"] = true, ["setpopupmenu"] = true, ["setposition"] = true, ["setsize"] = true, ["settop"] = true, ["setvisible"] = true, ["setwidth"] = true, ["update"] = true},
  },
  ["controlscrollbar"] = {
    parent = "object",
    props = {["increment"] = "word", ["page"] = "word", ["position"] = "integer", ["range"] = "integer", ["smooth"] = "boolean", ["tracking"] = "boolean", ["visible"] = "boolean"},
    indexed = {},
    methods = {},
  },
  ["criticalsection"] = {
    parent = "object",
    props = {},
    indexed = {},
    methods = {["enter"] = true, ["leave"] = true, ["tryenter"] = true},
  },
  ["custombufdataset"] = {
    parent = "dbdataset",
    props = {["changecount"] = "integer", ["filename"] = "string", ["indexname"] = "string", ["maxindexescount"] = "integer", ["packetrecords"] = "integer", ["readonly"] = "boolean", ["unidirectional"] = "boolean"},
    indexed = {},
    methods = {["applyupdates"] = true, ["cancelupdates"] = true, ["createdataset"] = true, ["loadfromfile"] = true, ["loadfromstream"] = true, ["savetofile"] = true, ["savetostream"] = true},
  },
  ["customconnection"] = {
    parent = "component",
    props = {["afterconnect"] = "function", ["afterdisconnect"] = "function", ["beforeconnect"] = "function", ["beforedisconnect"] = "function", ["connected"] = "boolean", ["loginprompt"] = "boolean"},
    indexed = {},
    methods = {["close"] = true, ["open"] = true},
  },
  ["customcontrol"] = {
    parent = "wincontrol",
    props = {["canvas"] = "the", ["onpaint"] = "an"},
    indexed = {},
    methods = {["getcanvas"] = true},
  },
  ["customsqlquery"] = {
    parent = "custombufdataset",
    props = {["prepared"] = "boolean", ["sqlconnection"] = "sqlconnection", ["sqltransaction"] = "sqltransaction"},
    indexed = {},
    methods = {["execsql"] = true, ["parambyname"] = true, ["prepare"] = true, ["rowsaffected"] = true, ["unprepare"] = true},
  },
  ["customtype"] = {
    parent = "object",
    props = {["customtypetype"] = "tcustomtypetype", ["functiontypename"] = "string", ["name"] = "string", ["script"] = "string", ["scriptusesfloat"] = "boolean"},
    indexed = {},
    methods = {["bytetabletovalue"] = true, ["getcustomtype"] = true, ["registercustomtypeautoassembler"] = true, ["registercustomtypelua"] = true, ["valuetobytetable"] = true},
  },
  ["d3dhook_fontmap"] = {
    parent = "d3dhook_texture",
    props = {},
    indexed = {},
    methods = {["changefont"] = true, ["gettextwidth"] = true},
  },
  ["d3dhook_renderobject"] = {
    parent = "object",
    props = {["alphablend"] = "float", ["centerx"] = "float", ["centery"] = "float", ["rotation"] = "float", ["visible"] = "boolean", ["x"] = "float", ["y"] = "float", ["zorder"] = "integer"},
    indexed = {},
    methods = {},
  },
  ["d3dhook_sprite"] = {
    parent = "d3dhook_renderobject",
    props = {["height"] = "integer", ["texture"] = "d3dhook_texture", ["width"] = "integer"},
    indexed = {},
    methods = {},
  },
  ["d3dhook_textcontainer"] = {
    parent = "d3dhook_renderobject",
    props = {["fontmap"] = "the", ["text"] = "the"},
    indexed = {},
    methods = {},
  },
  ["d3dhook_texture"] = {
    parent = "object",
    props = {["height"] = "integer", ["width"] = "integer"},
    indexed = {},
    methods = {["loadtexturebypicture"] = true},
  },
  ["database"] = {
    parent = "customconnection",
    props = {["connected"] = "boolean", ["databasename"] = "string", ["keepconnection"] = "boolean", ["params"] = "strings", ["transactioncount"] = "integer"},
    indexed = {},
    methods = {},
  },
  ["dataset"] = {
    parent = "component",
    props = {["active"] = "boolean", ["autocalcfields"] = "boolean", ["blockreadsize"] = "integer", ["bof"] = "boolean;", ["canmodify"] = "boolean", ["defaultfields"] = "boolean", ["eof"] = "boolean;", ["fieldcount"] = "integer;", ["fields"] = "fields", ["filter"] = "string", ["filtered"] = "boolean", ["filteroptions"] = "set", ["found"] = "boolean", ["isunidirectional"] = "boolean", ["modified"] = "boolean", ["recno"] = "integer", ["recordcount"] = "integer"},
    indexed = {["fieldvalues"] = "something"},
)MLUA"),
            std::string_view(R"MLUA(    methods = {["append"] = true, ["appendrecord"] = true, ["cancel"] = true, ["checkbrowsemode"] = true, ["clearfields"] = true, ["close"] = true, ["controlsdisabled"] = true, ["fieldbyname"] = true, ["findfield"] = true, ["findfirst"] = true, ["findlast"] = true, ["findnext"] = true, ["findprior"] = true, ["first"] = true, ["insert"] = true, ["isempty"] = true, ["last"] = true, ["locate"] = true, ["lookup"] = true, ["moveby"] = true, ["next"] = true, ["open"] = true, ["post"] = true, ["prior"] = true, ["refresh"] = true, ["updatecursorpos"] = true, ["updaterecord"] = true},
  },
  ["dbdataset"] = {
    parent = "dataset",
    props = {["database"] = "database", ["transaction"] = "dbtransaction"},
    indexed = {},
    methods = {},
  },
  ["dbtransaction"] = {
    parent = "component",
    props = {["active"] = "boolean", ["database"] = "database"},
    indexed = {},
    methods = {["closedatasets"] = true},
  },
  ["diagram"] = {
    parent = "customcontrol",
    props = {["allowusertochangeattachpoints"] = "boolean", ["allowusertocreateplotpoints"] = "boolean", ["allowusertomoveblocks"] = "boolean", ["allowusertomoveplotpoints"] = "boolean", ["allowusertoresizeblocks"] = "boolean", ["arrowstyles"] = "string", ["ascenter"] = "there", ["ascenterbetweenpoints"] = "there", ["asdestination"] = "there", ["asorigin"] = "there", ["aspoints"] = "there", ["autoside"] = "boolean", ["autosidedistance"] = "integer", ["autosize"] = "boolean", ["backgroundcolor"] = "integer", ["block"] = "diagramblock", ["blockbackground"] = "integer", ["blockcount"] = "integer", ["caption"] = "string", ["destinationblock"] = "dialogblock", ["destinationdescriptor"] = "blocksidedescriptortable", ["dragbody"] = "boolean", ["drawplotpoints"] = "boolean", ["height"] = "integer", ["linecolor"] = "integer", ["linethickness"] = "integer", ["linkcount"] = "integer", ["name"] = "string", ["ondblclick"] = "function", ["ondoubleclickbody"] = "function", ["ondoubleclickheader"] = "function", ["ondrag"] = "function", ["ondragend"] = "function", ["ondragstart"] = "funtion", ["onrenderbody"] = "function", ["onrenderheader"] = "function", ["originblock"] = "dialogblock", ["origindescriptor"] = "blocksidedescriptortable", ["owner"] = "diagram", ["plotpointcolor"] = "integer", ["pointcount"] = "integer", ["position"] = "integer", ["scrollx"] = "integer", ["scrolly"] = "integer", ["showheader"] = "boolean", ["side"] = "integer", ["strings"] = "strings", ["tag"] = "integer", ["textcolor"] = "integer", ["width"] = "integer", ["x"] = "integer", ["y"] = "integer", ["zoom"] = "float"},
    indexed = {["block"] = "diagramblock", ["link"] = "diagramlink", ["points"] = "table{x"},
    methods = {["addconnection"] = true, ["addpoint"] = true, ["createblock"] = true, ["executestub"] = true, ["getlinks"] = true, ["getobjectat"] = true, ["getpointindexat"] = true, ["haslinktoblock"] = true, ["intersectswithline"] = true, ["loadfromfile"] = true, ["loadfromstream"] = true, ["overlapswith"] = true, ["removeallpoints"] = true, ["reset"] = true, ["saveasimage"] = true, ["savetofile"] = true, ["savetostream"] = true, ["updateside"] = true},
  },
  ["disassembler"] = {
    parent = "object",
    props = {["address"] = "integer", ["bytes"] = "table", ["commentsoverride"] = "string", ["description"] = "string", ["iscall"] = "boolean", ["isconditionaljump"] = "boolean", ["isjump"] = "boolean", ["isrep"] = "boolean", ["isret"] = "boolean", ["lastdisassembledata"] = "table", ["modrmvalue"] = "integer", ["modrmvaluetype"] = "disassemblervaluetype", ["ondisassembleoverride"] = "function", ["onpostdisassemble"] = "function", ["opcode"] = "string", ["parameters"] = "string", ["parametervalue"] = "integer", ["parametervaluetype"] = "disassemblervaluetype", ["syntaxhighlighting"] = "boolean"},
    indexed = {},
    methods = {["decodelastparameterstostring"] = true, ["disassemble"] = true, ["getlastdisassembledata"] = true},
  },
  ["disassemblerview"] = {
    parent = "panel",
    props = {["hidefocusrect"] = "boolean", ["onextralinerender"] = "function", ["onselectionchange"] = "function", ["osb"] = "bitmap", ["selectedaddress"] = "integer", ["selectedaddress2"] = "integer", ["showjumplines"] = "boolean", ["spaceabovelines"] = "integer", ["spacebelowlines"] = "integer", ["topaddress"] = "integer"},
    indexed = {},
    methods = {},
  },
  ["disassemblerviewline"] = {
    parent = "object",
    props = {["address"] = "the", ["ondisassemblerviewoverride"] = "called", ["owner"] = "the"},
    indexed = {},
    methods = {["funtion"] = true},
  },
  ["dissectcode"] = {
    parent = "object",
    props = {},
    indexed = {},
    methods = {["addreference"] = true, ["clear"] = true, ["deletereference"] = true, ["dissect"] = true, ["getreferencedfunctions"] = true, ["getreferencedstrings"] = true, ["getreferences"] = true, ["loadfromfile"] = true, ["savetofile"] = true},
  },
  ["domdocument"] = {
    parent = "domnode_toplevel",
    props = {},
    indexed = {},
    methods = {},
  },
  ["domnode"] = {
    parent = nil,
    props = {},
    indexed = {},
    methods = {["writetofile"] = true, ["writetostream"] = true},
  },
  ["edit"] = {
    parent = "wincontrol",
    props = {["caretpos"] = "point", ["onchange"] = "function", ["onkeydown"] = "function", ["onkeypress"] = "function", ["onkeyup"] = "function", ["passwordchar"] = "string[1]", ["sellength"] = "number", ["selstart"] = "number", ["seltext"] = "string", ["text"] = "string"},
    indexed = {},
    methods = {["clear"] = true, ["clearselection"] = true, ["copytoclipboard"] = true, ["cuttoclipboard"] = true, ["getonchange"] = true, ["getonkeydown"] = true, ["getonkeypress"] = true, ["getonkeyup"] = true, ["getsellength"] = true, ["getselstart"] = true, ["getseltext"] = true, ["pastefromclipboard"] = true, ["select"] = true, ["selectall"] = true, ["selecttext"] = true, ["setonchange"] = true, ["setonkeydown"] = true, ["setonkeypress"] = true, ["setonkeyup"] = true},
  },
  ["event"] = {
    parent = "object",
    props = {},
    indexed = {},
    methods = {["resetevent"] = true, ["setevent"] = true, ["waitfor"] = true},
  },
  ["field"] = {
    parent = "component",
    props = {["datatype"] = "string", ["fieldname"] = "string", ["index"] = "integer", ["isnull"] = "boolean", ["value"] = "something"},
    indexed = {},
    methods = {},
  },
  ["fields"] = {
    parent = "inheritence: object",
    props = {["count"] = "integer"},
    indexed = {["fields"] = "field"},
    methods = {["add"] = true, ["clear"] = true, ["fieldbyname"] = true, ["fieldbynumber"] = true, ["indexof"] = true},
  },
  ["filedialog"] = {
    parent = "commondialog",
    props = {["defaultext"] = "string", ["filename"] = "string", ["files"] = "strings", ["filter"] = "string", ["filterindex"] = "integer", ["initialdir"] = "string"},
    indexed = {},
    methods = {},
  },
  ["filestream"] = {
    parent = "handlestream",
    props = {},
    indexed = {},
    methods = {["fmsharecompat"] = true},
  },
  ["finddialog"] = {
    parent = "commondialog",
    props = {["findtext"] = "string", ["onfind"] = "function", ["onhelp"] = "function", ["options"] = "enum"},
    indexed = {},
    methods = {},
  },
  ["font"] = {
    parent = "customfont",
    props = {["charset"] = "integer", ["color"] = "integer", ["height"] = "integer", ["name"] = "string", ["orientation"] = "integer", ["pitch"] = "string", ["quality"] = "string", ["size"] = "integer", ["style"] = "string"},
    indexed = {},
    methods = {["assign"] = true, ["getcolor"] = true, ["getname"] = true, ["getsize"] = true, ["setcolor"] = true, ["setname"] = true, ["setsize"] = true},
  },
  ["form"] = {
    parent = "scrollingwincontrol",
    props = {["allowdropfiles"] = "boolean", ["designtimeppi"] = "integer", ["formstate"] = "formstate", ["menu"] = "mainmenu", ["modalresult"] = "integer", ["onclose"] = "function", ["ondropfiles"] = "function"},
    indexed = {},
    methods = {["bringtofront"] = true, ["centerscreen"] = true, ["close"] = true, ["dragnow"] = true, ["fixdpi"] = true, ["getborderstyle"] = true, ["getmenu"] = true, ["getonclose"] = true, ["hide"] = true, ["isforegroundwindow"] = true, ["loadformposition"] = true, ["printtorasterimage"] = true, ["registerclosecallback"] = true, ["registercreatecallback"] = true, ["registerfirstshowcallback"] = true, ["saveformposition"] = true, ["setborderstyle"] = true, ["setmenu"] = true, ["setonclose"] = true, ["show"] = true, ["showmodal"] = true, ["unregisterclosecallback"] = true, ["unregistercreatecallback"] = true, ["unregisterfirstshowcallback"] = true},
  },
  ["generichotkey"] = {
    parent = "object",
    props = {["delaybetweenactivate"] = "integer", ["onhotkey"] = "the"},
    indexed = {},
    methods = {["getkeys"] = true, ["setkeys"] = true, ["setonhotkey"] = true},
  },
  ["graphic"] = {
    parent = "object",
    props = {["height"] = "integer", ["transparent"] = "boolean", ["width"] = "integer"},
    indexed = {},
    methods = {["getheight"] = true, ["getwidth"] = true, ["loadfromfile"] = true, ["savetofile"] = true, ["setheight"] = true, ["setwidth"] = true},
  },
  ["graphiccontrol"] = {
    parent = "control",
    props = {["canvas"] = "canvas"},
    indexed = {},
    methods = {["getcanvas"] = true},
  },
  ["groupbox"] = {
    parent = "wincontrol",
    props = {},
    indexed = {},
    methods = {},
  },
  ["headersection"] = {
    parent = "collectionitem",
    props = {["alignment"] = "string", ["imageindex"] = "imageindex", ["maxwidth"] = "integer", ["minwidth"] = "integer", ["originalindex"] = "integer", ["text"] = "string", ["visible"] = "boolean", ["width"] = "integer"},
    indexed = {},
    methods = {},
  },
  ["headersections"] = {
    parent = "collection",
    props = {},
    indexed = {},
    methods = {["add"] = true, ["delete"] = true, ["insert"] = true},
  },
  ["hexadecimal"] = {
    parent = "panel",
    props = {["address"] = "integer", ["displaytype"] = "displaytype", ["hasselection"] = "boolean", ["oncharacterrender"] = "function", ["onvaluerender"] = "function", ["selectionstart"] = "integer", ["selectionstop"] = "integer"},
    indexed = {},
    methods = {["onaddresschange"] = true, ["onbyteselect"] = true},
  },
  ["icon"] = {
    parent = "inheritence: custombitmap",
    props = {},
    indexed = {},
    methods = {},
  },
  ["image"] = {
    parent = "graphiccontrol",
    props = {["canvas"] = "canvas", ["picture"] = "picture", ["stretch"] = "boolean", ["transparent"] = "boolean"},
    indexed = {},
    methods = {["getcanvas"] = true, ["getpicture"] = true, ["getstretch"] = true, ["gettransparent"] = true, ["loadimagefromfile"] = true, ["setpicture"] = true, ["setstretch"] = true, ["settransparent"] = true},
  },
  ["imagelist"] = {
    parent = nil,
    props = {["count"] = "integer", ["drawingstyle"] = "'dsfocus'", ["height"] = "integer", ["masked"] = "boolean", ["onchange"] = "function", ["scaled"] = "boolean", ["width"] = "integer"},
    indexed = {},
    methods = {["add"] = true, ["draw"] = true, ["getbitmap"] = true},
  },
  ["internet"] = {
    parent = "object",
    props = {["header"] = "string"},
    indexed = {},
    methods = {["getinternet"] = true, ["geturl"] = true, ["posturl"] = true},
  },
  ["jpegimage"] = {
    parent = "inheritence: custombitmap",
    props = {},
    indexed = {},
    methods = {},
  },
  ["label"] = {
    parent = "graphiccontrol",
    props = {},
    indexed = {},
    methods = {},
  },
  ["listbox"] = {
    parent = "wincontrol",
    props = {["canvas"] = "canvas", ["itemindex"] = "integer", ["items"] = "strings", ["multiselect"] = "boolean"},
    indexed = {["selected"] = "unknown"},
    methods = {["clear"] = true, ["clearselection"] = true, ["getcanvas"] = true, ["getitemindex"] = true, ["getitems"] = true, ["selectall"] = true, ["setitemindex"] = true, ["setitems"] = true},
  },
  ["listcolumn"] = {
    parent = "collectionitem",
)MLUA"),
            std::string_view(R"MLUA(    props = {["autosize"] = "boolean", ["caption"] = "string", ["maxwidth"] = "integer", ["minwidth"] = "integer", ["visible"] = "boolean", ["width"] = "integer"},
    indexed = {},
    methods = {["getautosize"] = true, ["getcaption"] = true, ["getmaxwidth"] = true, ["getminwidth"] = true, ["getwidth"] = true, ["setautosize"] = true, ["setcaption"] = true, ["setmaxwidth"] = true, ["setminwidth"] = true, ["setwidth"] = true},
  },
  ["listcolumns"] = {
    parent = "collection",
    props = {},
    indexed = {["columns"] = "array"},
    methods = {["add"] = true, ["getcolumn"] = true, ["setcolumn"] = true},
  },
  ["listitem"] = {
    parent = "tobject",
    props = {["caption"] = "boolean", ["checked"] = "boolean", ["data"] = "integer", ["imageindex"] = "integer", ["index"] = "integer", ["owner"] = "listitems", ["selected"] = "boolean", ["stateindex"] = "integer", ["subitems"] = "strings"},
    indexed = {},
    methods = {["delete"] = true, ["displayrect"] = true, ["displayrectsubitem"] = true, ["getcaption"] = true, ["getchecked"] = true, ["getsubitems"] = true, ["makevisible"] = true, ["setcaption"] = true, ["setchecked"] = true},
  },
  ["listitems"] = {
    parent = "tobject",
    props = {["count"] = "integer"},
    indexed = {["item"] = "listitem[]"},
    methods = {["add"] = true, ["clear"] = true, ["getcount"] = true, ["getitem"] = true},
  },
  ["listview"] = {
    parent = "wincontrol",
    props = {["autowidthlastcolumn"] = "boolean", ["canvas"] = "canvas", ["columns"] = "listcolumns", ["hideselection"] = "boolean", ["itemindex"] = "integer", ["items"] = "listitems", ["largeimages"] = "imagelist", ["oncustomdraw"] = "function", ["oncustomdrawitem"] = "function", ["oncustomdrawsubitem"] = "function", ["ondata"] = "function", ["ownerdata"] = "boolean", ["rowselect"] = "boolean", ["selected"] = "listitem", ["smallimages"] = "imagelist", ["stateimages"] = "imagelist", ["topitem"] = "listitem", ["visiblerowcount"] = "integer"},
    indexed = {},
    methods = {["beginupdate"] = true, ["clear"] = true, ["endupdate"] = true, ["getcanvas"] = true, ["getcolumns"] = true, ["getitemat"] = true, ["getitemindex"] = true, ["getitems"] = true, ["setitemindex"] = true},
  },
  ["luapipe"] = {
    parent = "object",
    props = {["connected"] = "boolean:"},
    indexed = {},
    methods = {["lock"] = true, ["readbyte"] = true, ["readbytes"] = true, ["readdouble"] = true, ["readdword"] = true, ["readfloat"] = true, ["readqword"] = true, ["readstring"] = true, ["readwidestring"] = true, ["readword"] = true, ["unlock"] = true, ["writebyte"] = true, ["writebytes"] = true, ["writedouble"] = true, ["writedword"] = true, ["writefloat"] = true, ["writeqword"] = true, ["writestring"] = true, ["writewidestring"] = true, ["writeword"] = true},
  },
  ["luapipeclient"] = {
    parent = "luapipe",
    props = {},
    indexed = {},
    methods = {},
  },
  ["luapipeserver"] = {
    parent = "luapipe",
    props = {["address"] = "integer", ["handle"] = "integer", ["modulename"] = "string", ["name"] = "string", ["parameters"] = "string", ["path"] = "string", ["pid"] = "integer", ["returntype"] = "string", ["searchkey"] = "string", ["symbolsize"] = "integer", ["valid"] = "boolean"},
    indexed = {["value"] = "a"},
    methods = {["acceptconnection"] = true, ["addmodule"] = true, ["addsymbol"] = true, ["clear"] = true, ["createsymbollist"] = true, ["deletemodule"] = true, ["deletesymbol"] = true, ["enumregisteredsymbollists"] = true, ["function"] = true, ["getbinaryvalue"] = true, ["getmainsymbollist"] = true, ["getmodulelist"] = true, ["getsettings"] = true, ["getsymbolfromaddress"] = true, ["getsymbolfromstring"] = true, ["getsymbollist"] = true, ["register"] = true, ["reloadsettingsfromregistry"] = true, ["setbinaryvalue"] = true, ["unregister"] = true},
  },
  ["mainmenu"] = {
    parent = "menu",
    props = {},
    indexed = {},
    methods = {},
  },
  ["memo"] = {
    parent = "edit",
    props = {["canpaste"] = "boolean", ["canredo"] = "boolean", ["canundo"] = "boolean", ["charwidth"] = "integer", ["gutter"] = "gutter", ["lineheight"] = "integer", ["lines"] = "strings", ["readonly"] = "boolean", ["scrollbars"] = "scrollstyle", ["selend"] = "integer", ["selstart"] = "integer", ["seltext"] = "string", ["ssautoboth"] = "\"", ["ssautohorizontal"] = "same", ["ssautovertical"] = "\"", ["ssboth"] = "has", ["sshorizontal"] = "has", ["ssnone"] = "no", ["ssvertical"] = "has", ["wantreturns"] = "boolean", ["wanttabs"] = "boolean", ["wordwrap"] = "boolean"},
    indexed = {},
    methods = {["append"] = true, ["clearselection"] = true, ["clearundo"] = true, ["copytoclipboard"] = true, ["cuttoclipboard"] = true, ["getlines"] = true, ["getscrollbars"] = true, ["getwantreturns"] = true, ["getwanttabs"] = true, ["getwordwrap"] = true, ["marktextassaved"] = true, ["pastefromclipboard"] = true, ["redo"] = true, ["selectall"] = true, ["setscrollbars"] = true, ["setwantreturns"] = true, ["setwanttabs"] = true, ["setwordwrap"] = true, ["undo"] = true},
  },
  ["memoryrecordhotkey"] = {
    parent = "object",
    props = {["action"] = "integer", ["activatesound"] = "string", ["active"] = "boolean", ["address"] = "string", ["addressstring"] = "string", ["allowdecrease"] = "boolean", ["allowincrease"] = "boolean", ["async"] = "boolean", ["asyncprocessing"] = "boolean", ["asyncprocessingtime"] = "qword", ["collapsed"] = "boolean", ["color"] = "integer", ["count"] = "number", ["currentaddress"] = "integer", ["customtypename"] = "string", ["deactivatesound"] = "string", ["description"] = "string", ["displayasdropdownlistitem"] = "boolean", ["dohotkey"] = "executes", ["dontsave"] = "boolean", ["dropdowncount"] = "integer", ["dropdowndescriptiononly"] = "boolean", ["dropdownlinked"] = "boolean", ["dropdownlinkedmemrec"] = "string", ["dropdownlist"] = "stringlist", ["dropdownreadonly"] = "boolean", ["hasmouseover"] = "boolean", ["hotkeycount"] = "integer", ["hotkeystring"] = "string", ["id"] = "integer", ["index"] = "integer", ["isaddressgroupheader"] = "boolean", ["isgroupheader"] = "boolean", ["isreadable"] = "boolean", ["keys"] = "table", ["numericalvalue"] = "number", ["offsetcount"] = "integer", ["onactivate"] = "function", ["ondeactivate"] = "function", ["ondestroy"] = "function", ["ongetdisplayvalue"] = "function", ["onhotkey"] = "function", ["onposthotkey"] = "function", ["onvaluechanged"] = "function", ["onvaluechangedbyuser"] = "function", ["options"] = "string", ["owner"] = "memoryrecord", ["parent"] = "memoryrecord", ["script"] = "string", ["selected"] = "boolean", ["showashex"] = "boolean", ["showassigned"] = "boolean", ["type"] = "valuetype", ["value"] = "string", ["vartype"] = "valuetype"},
    indexed = {["child"] = "array", ["dropdowndescription"] = "array", ["dropdownvalue"] = "array", ["hotkey"] = "array", ["offset"] = "integer", ["offsettext"] = "string"},
    methods = {["appendtoentry"] = true, ["beginedit"] = true, ["createhotkey"] = true, ["disablewithoutexecute"] = true, ["endedit"] = true, ["getaddress"] = true, ["getcurrentaddress"] = true, ["getdescription"] = true, ["gethotkey"] = true, ["gethotkeybyid"] = true, ["getoffset"] = true, ["getoffsetcount"] = true, ["mrhactivate"] = true, ["mrhdeactivate"] = true, ["mrhdecreasevalue"] = true, ["mrhincreasevalue"] = true, ["mrhsetvalue"] = true, ["mrhtoggleactivation"] = true, ["mrhtoggleactivationallowdecrease"] = true, ["mrhtoggleactivationallowincrease"] = true, ["reinterpret"] = true, ["setaddress"] = true, ["setdescription"] = true, ["setoffset"] = true, ["setoffsetcount"] = true},
  },
  ["memorystream"] = {
    parent = "stream",
    props = {["memory"] = "integer"},
    indexed = {},
    methods = {["loadfromfile"] = true, ["loadfromfilenoerror"] = true, ["savetofile"] = true, ["savetofilenoerror"] = true},
  },
  ["memoryview"] = {
    parent = "form",
    props = {["disassemblerview"] = "the", ["hexadecimalview"] = "the"},
    indexed = {},
    methods = {},
  },
  ["memscan"] = {
    parent = "object",
    props = {["alignmentparam"] = "string", ["alignmenttype"] = "scan", ["binarystringasdecimal"] = "boolean", ["casesensitive"] = "boolean", ["codepage"] = "boolean;", ["comparetosavedscan"] = "boolean", ["count"] = "integer", ["customtype"] = "tcustomtype", ["fastscanmethod"] = "tfastscanmethod", ["fastscanparameter"] = "string", ["foundlist"] = "foundlist", ["fsmaligned"] = "the", ["fsmlastdigits"] = "the", ["fsmnotaligned"] = "no", ["hexadecimal"] = "boolean", ["input1"] = "if", ["input2"] = "if", ["iscasesensitive"] = "when", ["ishexadecimalinput"] = "when", ["isnotabinarystring"] = "when", ["ispercentage"] = "when", ["isunicodescan"] = "when", ["isunique"] = "boolean", ["lastscantype"] = "scantype", ["lastscanvalue"] = "string", ["lastscanwasregionscan"] = "boolean", ["memscan"] = "the", ["onguiupdate"] = "function", ["onlyoneresult"] = "boolean", ["onscandone"] = "function", ["percentage"] = "boolean", ["protectionflags"] = "see", ["result"] = "integer", ["roundingtype"] = "troundingtype", ["rtextremerounded"] = "rounded", ["rtrounded"] = "normal", ["rttruncated"] = "truncated", ["savedresultname"] = "string", ["savedscanname"] = "string", ["scancopyonwrite"] = "tscanregionpreference", ["scanexecutable"] = "tscanregionpreference", ["scanoption"] = "tscanoption", ["scanresultfolder"] = "string", ["scanvalue"] = "string", ["scanvalue1"] = "string", ["scanvalue2"] = "string", ["scanwritable"] = "tscanregionpreference", ["sobiggerthan"] = "bigger", ["sochanged"] = "changed", ["sodecreasedvalue"] = "decreased", ["sodecreasedvalueby"] = "decreased", ["soexactvalue"] = "exact", ["soincreasedvalue"] = "increased", ["soincreasedvalueby"] = "increased", ["sosmallerthan"] = "smaller", ["sounchanged"] = "unchanged", ["sounknownvalue"] = "unknown", ["sovaluebetween"] = "value", ["startaddress"] = "integer", ["stopaddress"] = "integer", ["utf16"] = "boolean", ["variabletype"] = "tvariabletype", ["vartype"] = "tvariabletype"},
    indexed = {["address"] = "string", ["value"] = "string"},
    methods = {["deinitialize"] = true, ["firstscan"] = true, ["getaddress"] = true, ["getattachedfoundlist"] = true, ["getcount"] = true, ["getonlyresult"] = true, ["getprogress"] = true, ["getsavedresulthandler"] = true, ["getsavedresultlist"] = true, ["getstringfromaddress"] = true, ["getvalue"] = true, ["initialize"] = true, ["newscan"] = true, ["nextscan"] = true, ["savecurrentresults"] = true, ["scan"] = true, ["setonlyoneresult"] = true, ["waittilldone"] = true},
  },
  ["menu"] = {
    parent = "component",
    props = {["items"] = "menuitem"},
    indexed = {},
    methods = {["getitems"] = true},
  },
  ["menuitem"] = {
    parent = "component",
    props = {["caption"] = "string", ["count"] = "integer", ["fontcolor"] = "color", ["imageindex"] = "integer", ["imagelist"] = "imagelist", ["menu"] = "menu", ["menuindex"] = "integer", ["onclick"] = "function", ["parent"] = "menuitem", ["shortcut"] = "string"},
    indexed = {["item"] = "array"},
    methods = {["add"] = true, ["clear"] = true, ["delete"] = true, ["doclick"] = true, ["getcaption"] = true, ["getcount"] = true, ["getitem"] = true, ["getonclick"] = true, ["getshortcut"] = true, ["insert"] = true, ["setcaption"] = true, ["setonclick"] = true, ["setshortcut"] = true},
  },
  ["multireadexclusivewritesynchronizer"] = {
    parent = "object",
    props = {},
    indexed = {},
    methods = {["beginread"] = true, ["beginwrite"] = true, ["endread"] = true, ["endwrite"] = true},
  },
  ["object"] = {
    parent = nil,
    props = {["classname"] = "string"},
    indexed = {},
    methods = {["destroy"] = true, ["fieldaddress"] = true, ["getclassname"] = true, ["methodaddress"] = true, ["methodname"] = true},
  },
  ["odbcconnection"] = {
    parent = "sqlconnection",
    props = {["databasename"] = "string", ["driver"] = "string", ["filedsn"] = "string"},
    indexed = {},
    methods = {},
  },
  ["opendialog"] = {
    parent = "filedialog",
)MLUA"),
            std::string_view(R"MLUA(    props = {["ofallowmultiselect"] = "allow", ["ofautopreview"] = "details", ["ofdontaddtorecent"] = "do", ["ofenablesizing"] = "dialog", ["offilemustexist"] = "shows", ["offorceshowhidden"] = "show", ["ofhidereadonly"] = "hide", ["ofnochangedir"] = "do", ["ofnodereferencelinks"] = "do", ["ofnoreadonlyreturn"] = "do", ["ofoverwriteprompt"] = "if", ["ofpathmustexist"] = "shows", ["ofshowhelp"] = "show", ["ofviewdetail"] = "details", ["options"] = "string"},
    indexed = {},
    methods = {},
  },
  ["pagecontrol"] = {
    parent = "wincontrol",
    props = {["activepage"] = "tabsheet", ["pagecount"] = "integer", ["showtabs"] = "boolean", ["tabindex"] = "integer"},
    indexed = {["page"] = "tabsheet"},
    methods = {["addtab"] = true, ["createpagecontrol"] = true, ["tabrect"] = true},
  },
  ["paintbox"] = {
    parent = "graphiccontrol",
    props = {},
    indexed = {},
    methods = {},
  },
  ["panel"] = {
    parent = "customcontrol",
    props = {["alignment"] = "alignment", ["bevelinner"] = "panelbevel", ["bevelouter"] = "panelbevel", ["bevelwidth"] = "integer", ["fullrepaint"] = "boolean"},
    indexed = {},
    methods = {["getalignment"] = true, ["getbevelinner"] = true, ["getbevelouter"] = true, ["getbevelwidth"] = true, ["getfullrepaint"] = true, ["setalignment"] = true, ["setbevelinner"] = true, ["setbevelouter"] = true, ["setbevelwidth"] = true, ["setfullrepaint"] = true},
  },
  ["param"] = {
    parent = "inheritence: collectionitem",
    props = {["datatype"] = "string", ["isnull"] = "boolean", ["name"] = "string", ["value"] = "something"},
    indexed = {},
    methods = {},
  },
  ["params"] = {
    parent = "inheritence: collection",
    props = {},
    indexed = {["items"] = "param"},
    methods = {["addparam"] = true},
  },
  ["pen"] = {
    parent = "custompen",
    props = {["color"] = "integer", ["width"] = "integer"},
    indexed = {},
    methods = {["getcolor"] = true, ["getwidth"] = true, ["setcolor"] = true, ["setwidth"] = true},
  },
  ["picture"] = {
    parent = "object",
    props = {},
    indexed = {},
    methods = {["assign"] = true, ["loadfromfile"] = true, ["loadfromstream"] = true, ["savetofile"] = true},
  },
  ["popupmenu"] = {
    parent = "menu",
    props = {},
    indexed = {},
    methods = {},
  },
  ["portablenetworkgraphic"] = {
    parent = "inheritence: custombitmap",
    props = {},
    indexed = {},
    methods = {},
  },
  ["progressbar"] = {
    parent = "wincontrol",
    props = {["max"] = "integer", ["min"] = "integer", ["position"] = "integer", ["step"] = "integer"},
    indexed = {},
    methods = {["getmax"] = true, ["getmin"] = true, ["getposition"] = true, ["setmax"] = true, ["setmin"] = true, ["setposition"] = true, ["setposition2"] = true, ["stepby"] = true, ["stepit"] = true},
  },
  ["radiogroup"] = {
    parent = "groupbox",
    props = {["columns"] = "integer", ["itemindex"] = "integer", ["items"] = "strings", ["onclick"] = "called"},
    indexed = {},
    methods = {["getcolumns"] = true, ["getitemindex"] = true, ["getitems"] = true, ["getonclick"] = true, ["getrows"] = true, ["setcolumns"] = true, ["setitemindex"] = true, ["setonclick"] = true},
  },
  ["rasterimage"] = {
    parent = "graphic",
    props = {["canvas"] = "canvas", ["pixelformat"] = "pixelformat", ["transparentcolor"] = "integer"},
    indexed = {},
    methods = {["getcanvas"] = true, ["getpixelformat"] = true, ["gettransparentcolor"] = true, ["loadfromstream"] = true, ["savetostream"] = true, ["settransparentcolor"] = true},
  },
  ["region"] = {
    parent = "graphicsobject",
    props = {},
    indexed = {},
    methods = {["addpolygon"] = true, ["addrectangle"] = true},
  },
  ["remotethread"] = {
    parent = "-",
    props = {["entrypoint"] = "integer", ["exports"] = "table", ["loaded"] = "boolean", ["result"] = "the"},
    indexed = {},
    methods = {["waitforthread"] = true},
  },
  ["riprelativescanner"] = {
    parent = "object",
    props = {["count"] = "integer"},
    indexed = {["address"] = "integer"},
    methods = {},
  },
  ["savedialog"] = {
    parent = "opendialog",
    props = {},
    indexed = {},
    methods = {},
  },
  ["scrollingwincontrol"] = {
    parent = "customcontrol",
    props = {["horzscrollbar"] = "controlscrollbar", ["vertscrollbar"] = "controlscrollbar"},
    indexed = {},
    methods = {},
  },
  ["selectdirectorydialog"] = {
    parent = "opendialog",
    props = {["position"] = "integer", ["size"] = "integer"},
    indexed = {},
    methods = {["copyfrom"] = true, ["read"] = true, ["readansistring"] = true, ["readbyte"] = true, ["readdword"] = true, ["readqword"] = true, ["readword"] = true, ["write"] = true, ["writeansistring"] = true, ["writebyte"] = true, ["writedword"] = true, ["writeqword"] = true, ["writeword"] = true},
  },
  ["semaphore"] = {
    parent = "object",
    props = {},
    indexed = {},
    methods = {["acquire"] = true, ["release"] = true},
  },
  ["splitter"] = {
    parent = "customcontrol",
    props = {},
    indexed = {},
    methods = {},
  },
  ["sqlconnection"] = {
    parent = "database",
    props = {["charset"] = "string", ["hostname"] = "string", ["options"] = "string", ["password"] = "string", ["transaction"] = "sqltransaction", ["username"] = "string"},
    indexed = {},
    methods = {["endtransaction"] = true, ["executedirect"] = true, ["gettablenames"] = true, ["starttransaction"] = true},
  },
  ["sqlite3connection"] = {
    parent = "sqlconnection",
    props = {},
    indexed = {},
    methods = {["createdb"] = true, ["dropdb"] = true, ["getinsertid"] = true},
  },
  ["sqlquery"] = {
    parent = "customsqlquery",
    props = {["database"] = "database", ["deletesql"] = "stringlist", ["insertsql"] = "stringlist", ["options"] = "string", ["paramcheck"] = "boolean", ["params"] = "params", ["parsesql"] = "boolean", ["readonly"] = "boolean", ["refreshsql"] = "stringlist", ["schematype"] = "string", ["sql"] = "string", ["state"] = "0", ["statementtype"] = "string", ["updatemode"] = "string", ["updatesql"] = "stringlist", ["useprimarykeyaskey"] = "boolean"},
    indexed = {},
    methods = {},
  },
  ["sqltransaction"] = {
    parent = "inheritence: dbtransaction",
    props = {["action"] = "string", ["options"] = "string", ["params"] = "stringlist", ["sqlconnection"] = "sqlconnection"},
    indexed = {},
    methods = {["commit"] = true, ["commitretaining"] = true, ["endtransaction"] = true, ["rollback"] = true, ["rollbackretaining"] = true, ["starttransaction"] = true},
  },
  ["stringlist"] = {
    parent = "strings",
    props = {["casesensitive"] = "boolean", ["duplicates"] = "duplicatestype", ["sorted"] = "boolean"},
    indexed = {},
    methods = {["getcasesensitive"] = true, ["getduplicates"] = true, ["getsorted"] = true, ["setcasesensitive"] = true, ["setduplicates"] = true, ["setsorted"] = true},
  },
  ["strings"] = {
    parent = "object",
    props = {["count"] = "integer", ["linebreak"] = "string", ["text"] = "string"},
    indexed = {["data"] = "integer", ["string"] = "string"},
    methods = {["add"] = true, ["addtext"] = true, ["beginupdate"] = true, ["clear"] = true, ["delete"] = true, ["endupdate"] = true, ["getcount"] = true, ["getdata"] = true, ["getstring"] = true, ["gettext"] = true, ["indexof"] = true, ["insert"] = true, ["loadfromfile"] = true, ["remove"] = true, ["savetofile"] = true, ["setdata"] = true, ["setstring"] = true, ["settext"] = true},
  },
  ["stringstream"] = {
    parent = "stream",
    props = {},
    indexed = {},
    methods = {},
  },
  ["structure"] = {
    parent = "object",
    props = {["count"] = "integer", ["name"] = "string", ["size"] = "integer"},
    indexed = {["element"] = "structureelement"},
    methods = {["addelement"] = true, ["addtoglobalstructurelist"] = true, ["autoguess"] = true, ["beginupdate"] = true, ["endupdate"] = true, ["fillfromdotnetaddress"] = true, ["getelement"] = true, ["getelementbyoffset"] = true, ["getname"] = true, ["removefromglobalstructurelist"] = true, ["setname"] = true},
  },
  ["structureelement"] = {
    parent = "object",
    props = {["attachwindow"] = "type=form", ["bytesize"] = "integer", ["childstruct"] = "structure", ["childstructstart"] = "integer", ["disabledzbuffer"] = "boolean", ["displaymethod"] = "string", ["extraparameters"] = "type=string", ["hasclosebutton"] = "type=boolean", ["height"] = "integer:", ["logfpu"] = "boolean", ["logstack"] = "boolean", ["mouseclip"] = "boolean", ["name"] = "string", ["newaf"] = "integer", ["newcf"] = "integer", ["newof"] = "integer", ["newpf"] = "integer", ["newr10"] = "integer", ["newr11"] = "integer", ["newr12"] = "integer", ["newr13"] = "integer", ["newr14"] = "integer", ["newr15"] = "integer", ["newr8"] = "integer", ["newr9"] = "integer", ["newrax"] = "integer", ["newrbp"] = "integer", ["newrbx"] = "integer", ["newrcx"] = "integer", ["newrdi"] = "integer", ["newrdx"] = "integer", ["newrip"] = "integer", ["newrsi"] = "integer", ["newrsp"] = "integer", ["newsf"] = "integer", ["newzf"] = "integer", ["offset"] = "integer", ["onclick"] = "function", ["onkeydown"] = "function", ["owner"] = "structure", ["percentageshown"] = "you", ["position"] = "type=integer", ["vartype"] = "integer", ["width"] = "integer", ["wireframemode"] = "boolean", ["yoururl"] = "type=string:"},
    indexed = {},
    methods = {["beginupdate"] = true, ["createfontmap"] = true, ["createsprite"] = true, ["createtextcontainer"] = true, ["createtexture"] = true, ["enableconsole"] = true, ["endupdate"] = true, ["function"] = true, ["getbytesize"] = true, ["getchildstruct"] = true, ["getchildstructstart"] = true, ["getname"] = true, ["getoffset"] = true, ["getownerstructure"] = true, ["getvalue"] = true, ["getvaluefrombase"] = true, ["getvartype"] = true, ["setbytesize"] = true, ["setchildstruct"] = true, ["setchildstructstart"] = true, ["setname"] = true, ["setoffset"] = true, ["setvalue"] = true, ["setvaluefrombase"] = true, ["setvartype"] = true},
  },
  ["tablefile"] = {
    parent = "object",
    props = {["initialized"] = "boolean", ["isplaying"] = "boolean", ["name"] = "string", ["stream"] = "memorystream"},
    indexed = {},
    methods = {["delete"] = true, ["getdata"] = true, ["pause"] = true, ["playxm"] = true, ["resume"] = true, ["savetofile"] = true, ["setvolume"] = true, ["stop"] = true},
  },
  ["tabsheet"] = {
    parent = "wincontrol",
    props = {["tabindex"] = "integer"},
    indexed = {},
    methods = {},
  },
  ["tfrmautoinject"] = {
    parent = "form",
    props = {["assemblescreen"] = "synedit", ["tabcount"] = "integer"},
    indexed = {["tabscript"] = "string"},
    methods = {["addtab"] = true, ["deletetab"] = true},
  },
  ["tfrmcodefilter"] = {
    parent = "form",
    props = {},
    indexed = {},
    methods = {["isinlist"] = true},
  },
  ["tfrmluaengine"] = {
    parent = "form",
    props = {["moutput"] = "memo", ["mscript"] = "synedit"},
    indexed = {},
    methods = {},
  },
  ["tfrmtracer"] = {
    parent = "form",
    props = {["address"] = "integer", ["context"] = "contexttable", ["count"] = "integer", ["hasstacksnapshot"] = "boolean", ["instruction"] = "string", ["instructionsize"] = "integer", ["referencedaddress"] = "integer", ["referenceddata"] = "bytearray", ["selected"] = "boolean", ["selectioncount"] = "integer"},
    indexed = {["entry"] = "table", ["stackentry"] = "bytearray"},
    methods = {},
  },
  ["tfrmultimap2"] = {
    parent = "form",
    props = {["count"] = "integer"},
    indexed = {},
    methods = {["isinlist"] = true},
  },
  ["thread"] = {
    parent = "object",
    props = {["finished"] = "boolean", ["name"] = "string", ["result"] = "string", ["terminated"] = "boolean"},
    indexed = {},
    methods = {["freeonterminate"] = true, ["resume"] = true, ["suspend"] = true, ["synchronize"] = true, ["terminate"] = true, ["waitfor"] = true},
  },
  ["timer"] = {
    parent = "component",
    props = {["enabled"] = "boolean", ["interval"] = "integer", ["ontimer"] = "function"},
    indexed = {},
)MLUA"),
            std::string_view(R"MLUA(    methods = {["getenabled"] = true, ["getinterval"] = true, ["getontimer"] = true, ["setenabled"] = true, ["setinterval"] = true, ["setontimer"] = true},
  },
  ["togglebox"] = {
    parent = "checkbox",
    props = {},
    indexed = {},
    methods = {},
  },
  ["trackbar"] = {
    parent = "wincontrol",
    props = {["max"] = "integer", ["min"] = "integer", ["onchange"] = "function", ["position"] = "integer"},
    indexed = {},
    methods = {["getmax"] = true, ["getmin"] = true, ["getonchange"] = true, ["getposition"] = true, ["setmax"] = true, ["setmin"] = true, ["setonchange"] = true, ["setposition"] = true},
  },
  ["treenode"] = {
    parent = "tobject",
    props = {["absoluteindex"] = "integer", ["count"] = "integer", ["data"] = "pointer", ["expanded"] = "boolean", ["haschildren"] = "boolean", ["imageindex"] = "integer", ["index"] = "integer", ["level"] = "integer", ["multiselected"] = "boolean", ["parent"] = "treenode", ["selected"] = "boolean", ["text"] = "string"},
    indexed = {["items"] = "treenode"},
    methods = {["add"] = true, ["collapse"] = true, ["delete"] = true, ["deletechildren"] = true, ["expand"] = true, ["getdisplayrect"] = true, ["getnextsibling"] = true, ["makevisible"] = true},
  },
  ["treenodes"] = {
    parent = "tobject",
    props = {["count"] = "integer"},
    indexed = {["item"] = "treenode"},
    methods = {["add"] = true, ["clear"] = true, ["getcount"] = true, ["getitem"] = true, ["insert"] = true, ["insertbehind"] = true},
  },
  ["treeview"] = {
    parent = "customcontrol",
    props = {["items"] = "treenodes", ["selected"] = "treenode"},
    indexed = {},
    methods = {["beginupdate"] = true, ["endupdate"] = true, ["fullcollapse"] = true, ["fullexpand"] = true, ["getitems"] = true, ["getselected"] = true, ["loadfromfile"] = true, ["savetofile"] = true, ["setselected"] = true},
  },
  ["virtualtreecolumn"] = {
    parent = "collectionitem",
    props = {["animationoptions"] = "comma", ["autooptions"] = "comma", ["autoresize"] = "boolean", ["autosizeindex"] = "integer", ["columns"] = "virtualtreecolunmns", ["focusedcolumn"] = "integer", ["focusednode"] = "node", ["fullrowselect"] = "boolean", ["index"] = "integer", ["miscoptions"] = "comma", ["nodedatasize"] = "integer", ["ondrawtext"] = "function", ["onexpanding"] = "function", ["onfreenode"] = "function", ["ongettext"] = "function", ["oninitnode"] = "function", ["onpainttext"] = "function", ["options"] = "set", ["paintoptions"] = "comma", ["selectionoptions"] = "comma", ["stringoptions"] = "comma", ["text"] = "string", ["treeoptions"] = "stringtreeoptions", ["visible"] = "shortcut"},
    indexed = {["expanded"] = "boolean", ["haschildren"] = "boolean", ["nodeheight"] = "height", ["nodeparent"] = "node", ["selected"] = "boolean"},
    methods = {["absoluteindex"] = true, ["add"] = true, ["addchild"] = true, ["addtoselection"] = true, ["beginupdate"] = true, ["clear"] = true, ["deletenode"] = true, ["deleteselectednodes"] = true, ["endupdate"] = true, ["enumcheckednodes"] = true, ["enumchildren"] = true, ["enumselectednodes"] = true, ["getfirstchild"] = true, ["getnextsibling"] = true, ["getnodedata"] = true, ["getnodedataasinteger"] = true, ["getnodedatapointer"] = true, ["getnodeparent"] = true, ["getrootnode"] = true, ["loadfromfile"] = true, ["nodechecked"] = true, ["nodeselected"] = true, ["removefromselection"] = true, ["savetofile"] = true, ["setnodedata"] = true, ["setnodedataasinteger"] = true},
  },
  ["wincontrol"] = {
    parent = "control",
    props = {["controlcount"] = "integer", ["doublebuffered"] = "boolean", ["handle"] = "integer", ["onenter"] = "function", ["onexit"] = "function"},
    indexed = {["control"] = "control"},
    methods = {["canfocus"] = true, ["focused"] = true, ["getcontrol"] = true, ["getcontrolatpos"] = true, ["getcontrolcount"] = true, ["getonenter"] = true, ["getonexit"] = true, ["setfocus"] = true, ["setlayeredattributes"] = true, ["setonenter"] = true, ["setonexit"] = true, ["setshape"] = true},
  },
  ["writelog"] = {
    parent = "inheritence: -",
    props = {["logsize"] = "integer", ["status"] = "boolean"},
    indexed = {},
    methods = {["getlog"] = true},
  },
  ["xmldocument"] = {
    parent = "domdocument",
    props = {},
    indexed = {},
    methods = {},
  },
}
)MLUA"),
        };
        constexpr std::array<std::string_view, 9> kScript2 = {
            std::string_view(R"MLUA(local classes = __ceClasses or {}
__ceClasses = nil

local lower = string.lower
local D = setmetatable({}, { __mode = "k" })
local Meta = {}
local Impl, Get, Set, Defaults, IndexGet, IndexSet, Init, Indexed = {}, {}, {}, {}, {}, {}, {}, {}
local forms = {}
local formNotifications = {}

local function touchMenus()
  local f = rawget(_G, "__manifoldMenusChanged")
  if f then
    f()
  end
end

local function touchForms()
  local f = rawget(_G, "__manifoldFormsChanged")
  if f then
    f()
  end
end

local layout = {}

local ExtraParents = {
  customcontrol = "wincontrol", customapplication = "component", tobject = "object", custompen = "object",
  custombrush = "object", customfont = "object", canvashelper = "object", customcanvas = "object",
  custombitmap = "rasterimage", handlestream = "stream", stream = "object", lclcomponent = "component",
  persistent = "object", graphicsobject = "object", phantom = "panel", mainform = "form",
  tfrmluaengine = "form", memoryview = "form", cefrm = "form", png = "bitmap", jpeg = "bitmap", icon = "bitmap",
  synedit = "memo", headercontrol = "panel", thread = "object", scrollbox = "scrollingwincontrol",
  pagecontrol = "wincontrol", tabsheet = "wincontrol", statusbar = "wincontrol", constraints = "object",
  borderspacing = "object", childsizing = "object", anchorside = "object", gutter = "object", point = "object",
}

local ClassNames = {
  object = "TObject", component = "TComponent", control = "TControl", wincontrol = "TWinControl", form = "TForm", cefrm = "TCEForm",
  mainform = "TMainForm", label = "TLabel", edit = "TEdit", memo = "TMemo", button = "TButton", checkbox = "TCheckBox",
  togglebox = "TToggleBox", combobox = "TComboBox", listbox = "TListBox", listview = "TListView", treeview = "TTreeView",
  panel = "TPanel", groupbox = "TGroupBox", radiogroup = "TRadioGroup", image = "TImage", paintbox = "TPaintBox",
  splitter = "TSplitter", progressbar = "TProgressBar", trackbar = "TTrackBar", mainmenu = "TMainMenu", popupmenu = "TPopupMenu",
  menuitem = "TMenuItem", imagelist = "TImageList", bitmap = "TBitmap", png = "TPortableNetworkGraphic", jpeg = "TJPEGImage",
  icon = "TIcon", picture = "TPicture", stringlist = "TStringList", strings = "TStrings", font = "TFont", canvas = "TCanvas",
  brush = "TBrush", pen = "TPen", opendialog = "TOpenDialog", savedialog = "TSaveDialog", selectdirectorydialog = "TSelectDirectoryDialog",
  colordialog = "TColorDialog", fontdialog = "TFontDialog", finddialog = "TFindDialog", listitems = "TListItems", listitem = "TListItem",
  listcolumns = "TListColumns", listcolumn = "TListColumn", treenodes = "TTreeNodes", treenode = "TTreeNode", application = "TApplication",
  thread = "TThread", phantom = "TControl", tfrmluaengine = "TfrmLuaEngine", memoryview = "TMemoryBrowser",
  disassemblerview = "TDisassemblerview", hexadecimal = "THexView", addresslist = "TAddresslist", synedit = "TSynEdit",
  headercontrol = "THeaderControl", scrollbox = "TScrollBox", pagecontrol = "TPageControl", tabsheet = "TTabSheet", statusbar = "TStatusBar",
}

local ObjectTypes = {
  font = "font", strings = "stringlist", stringlist = "stringlist", tstrings = "stringlist", canvas = "canvas", brush = "brush", pen = "pen",
  picture = "picture", listitems = "listitems", listcolumns = "listcolumns", treenodes = "treenodes", controlscrollbar = "controlscrollbar",
  bitmap = "bitmap", graphic = "bitmap", rasterimage = "bitmap", headersections = "headersections", constraints = "constraints",
  borderspacing = "borderspacing", gutter = "gutter", point = "point",
}

local Scalars = {
  string = "", integer = 0, number = 0, boolean = false, cardinal = 0, int64 = 0, word = 0, byte = 0, double = 0, single = 0,
  float = 0, ptruint = 0, qword = 0, dword = 0, longint = 0, smallint = 0, real = 0, int = 0, bool = false, ["string;"] = "",
  ["boolean;"] = false, ["integer;"] = 0, pointer = 0, rgbinteger = 0, modalresult = 0, imageindex = -1, height = 0, shortcut = "",
  alignmentoption = 0, alignment = 0, formstate = 0, panelbevel = 0, scrollstyle = 0, pixelformat = 0, checkboxstate = 0,
  duplicatestype = 0, displaytype = 0, troundingtype = 0, tfastscanmethod = 0, tscanoption = 0, scantype = 0, valuetype = 0,
  tvariabletype = 0, enum = 0, set = "[]", tscanregionpreference = 0, disassemblervaluetype = 0, colordefinition = 0x20000000,
}

local lineageCache = {}

local function parentOf(name)
  local info = classes[name]
  if info and info.parent and info.parent ~= "" and not info.parent:find(":") then
    return info.parent
  end
  return ExtraParents[name]
end

local function lineage(name)
  local cached = lineageCache[name]
  if cached then
    return cached
  end
  local list, seen, current = {}, {}, name
  while current and not seen[current] do
    seen[current] = true
    list[#list + 1] = current
    current = parentOf(current)
  end
  if not seen.object then
    list[#list + 1] = "object"
  end
  lineageCache[name] = list
  return list
end

local function isObject(value)
  return type(value) == "table" and D[value] ~= nil
end

local function inherits(value, name)
  local d = type(value) == "table" and D[value]
  if not d then
    return false
  end
  for _, class in ipairs(lineage(d.class)) do
    if class == name then
      return true
    end
  end
  return false
end

local function bind(obj, fn)
  return function(first, ...)
    if rawequal(first, obj) then
      return fn(obj, ...)
    end
    return fn(obj, first, ...)
  end
end

local function call(fn, ...)
  if type(fn) == "function" then
    return fn(...)
  end
end

local function removeValue(list, value)
  for i = #list, 1, -1 do
    if rawequal(list[i], value) then
      table.remove(list, i)
      return i
    end
  end
end

local serial = 0

local function new(class, owner)
  local obj = setmetatable({}, Meta)
  serial = serial + 1
  local d = { class = class, props = {}, components = {}, controls = {}, serial = serial }
  D[obj] = d
  if isObject(owner) then
    d.owner = owner
    local list = D[owner].components
    list[#list + 1] = obj
  end
  local chain = lineage(class)
  for i = #chain, 1, -1 do
    local init = Init[chain[i]]
    if init then
      init(obj, d)
    end
  end
  if layout.created then
    layout.created(obj, d)
  end
  return obj
end

local function setParent(obj, parent)
  local d = D[obj]
  local old = d.parent
  if old and D[old] then
    removeValue(D[old].controls, obj)
  end
  d.parent = nil
  if isObject(parent) then
    d.parent = parent
    local list = D[parent].controls
    list[#list + 1] = obj
  end
  if layout.parentChanged and old ~= d.parent then
    layout.parentChanged(obj, old, d.parent)
  end
  touchForms()
end

local function findOwned(obj, name, deep)
  local d = D[obj]
  if not d then
    return nil
  end
  for _, child in ipairs(d.components) do
    local childName = D[child] and D[child].props.name
    if childName and lower(childName) == name then
      return child
    end
  end
  for _, child in ipairs(d.controls) do
    local childName = D[child] and D[child].props.name
    if childName and lower(childName) == name then
      return child
    end
  end
  if deep then
    for _, child in ipairs(d.components) do
      local found = findOwned(child, name, true)
      if found then
        return found
      end
    end
  end
  return nil
end

local noop = function() end
local genericCache = {}

local function generic(k)
  local cached = genericCache[k]
  if cached then
    return cached
  end
  local fn = noop
  local prefix = k:sub(1, 3)
  if #k > 3 and prefix == "get" then
    local prop = k:sub(4)
    fn = function(self, index)
      if index ~= nil then
        local holder = self[prop]
        if type(holder) == "table" then
          return holder[index]
        end
      end
      return self[prop]
    end
  elseif #k > 3 and prefix == "set" then
    local prop = k:sub(4)
    fn = function(self, a, b)
      if b ~= nil then
        local holder = self[prop]
        if type(holder) == "table" then
          holder[a] = b
          return
        end
      end
      self[prop] = a
    end
  elseif #k > 2 and k:sub(1, 2) == "is" then
    local prop = k:sub(3)
    fn = function(self)
      return self[prop] and true or false
    end
  end
  genericCache[k] = fn
  return fn
end

local knownMethods = {}
for _, info in pairs(classes) do
  for name in pairs(info.methods or {}) do
    knownMethods[name] = true
  end
end

local function indexer(obj, name, handler)
  return setmetatable({}, {
    __index = function(_, i)
      return handler.get(obj, D[obj], i)
    end,
    __newindex = function(_, i, v)
      if handler.set then
        handler.set(obj, D[obj], i, v)
      end
    end,
    __len = function()
      return handler.len and handler.len(obj, D[obj]) or 0
    end,
  })
end

local function typedDefault(obj, d, k, kind)
  local scalar = Scalars[kind]
  if scalar ~= nil then
    return scalar
  end
  if kind:find("color") then
    return 0x20000000
  end
  local objectClass = ObjectTypes[kind]
  if objectClass then
    local value = new(objectClass, obj)
    d.props[k] = value
    return value
  end
  return nil
end

Meta.__index = function(obj, key)
  local d = D[obj]
  if d == nil then
    return nil
  end
  local chain = lineage(d.class)
  if type(key) == "number" then
    for i = 1, #chain do
      local f = IndexGet[chain[i]]
      if f then
        return f(obj, d, key)
      end
    end
    return nil
  end
  if type(key) ~= "string" then
    return nil
  end
  local k = lower(key)
  local value = d.props[k]
  if value ~= nil then
    return value
  end
  for i = 1, #chain do
    local class = chain[i]
    local getter = Get[class] and Get[class][k]
    if getter then
      return getter(obj, d)
    end
    local method = Impl[class] and Impl[class][k]
    if method then
      return bind(obj, method)
    end
    local defaults = Defaults[class]
    if defaults then
      local default = defaults[k]
      if default ~= nil then
        if type(default) == "function" then
          local made = default(obj, d)
          d.props[k] = made
          return made
        end
        return default
      end
    end
    local handlers = Indexed[class] and Indexed[class][k]
    if handlers then
      return indexer(obj, k, handlers)
    end
  end
  local child = findOwned(obj, k, false)
  if child then
    return child
  end
  for i = 1, #chain do
    local info = classes[chain[i]]
    if info then
      local kind = info.props[k]
      if kind then
        return typedDefault(obj, d, k, kind)
      end
      if info.indexed[k] then
        return indexer(obj, k, { get = function() return nil end })
      end
    end
  end
  for i = 1, #chain do
    local info = classes[chain[i]]
    if info and info.methods[k] then
      return bind(obj, generic(k))
    end
  end
  if knownMethods[k] then
    return bind(obj, generic(k))
  end
  if k:sub(1, 2) == "on" then
    return nil
  end
  if key:match("^%u") then
    local phantom = new("phantom", obj)
    D[phantom].props.name = key
    d.props[k] = phantom
    return phantom
  end
  if #k > 3 and (k:sub(1, 3) == "get" or k:sub(1, 3) == "set") then
    return bind(obj, generic(k))
  end
  return nil
end

Meta.__newindex = function(obj, key, value)
  local d = D[obj]
  if d == nil then
    return
  end
  if d.class == "menuitem" then
    touchMenus()
  end
  touchForms()
  local chain = lineage(d.class)
  if type(key) == "number" then
    for i = 1, #chain do
      local f = IndexSet[chain[i]]
      if f then
        return f(obj, d, key, value)
      end
    end
    return
  end
  local k = lower(tostring(key))
  local handled = false
  for i = 1, #chain do
    local setter = Set[chain[i]] and Set[chain[i]][k]
    if setter then
      setter(obj, d, value)
      handled = true
      break
    end
  end
  if not handled then
    d.props[k] = value
  end
  if layout.changed then
    layout.changed(obj, d, k)
  end
end

Meta.__tostring = function(obj)
  local d = D[obj]
)MLUA"),
            std::string_view(R"MLUA(  return string.format("%s: %016X", ClassNames[d and d.class or "object"] or "TObject", 0x10000000 + (d and d.serial or 0) * 16)
end

Meta.__concat = function(a, b)
  return tostring(a) .. tostring(b)
end

local function className(d)
  return ClassNames[d.class] or ("T" .. d.class:sub(1, 1):upper() .. d.class:sub(2))
end

local function destroy(obj)
  local d = D[obj]
  if not d or d.destroyed then
    return
  end
  call(d.props.ondestroy, obj)
  d.destroyed = true
  if d.class == "canvas" and layout.canvasFreed then
    layout.canvasFreed(d)
  end
  for i = #d.components, 1, -1 do
    destroy(d.components[i])
  end
  if d.owner and D[d.owner] then
    removeValue(D[d.owner].components, obj)
  end
  if d.parent and D[d.parent] then
    removeValue(D[d.parent].controls, obj)
    if layout.parentChanged then
      layout.parentChanged(obj, d.parent, nil)
    end
  end
  if d.parentItem and D[d.parentItem] then
    removeValue(D[d.parentItem].items, obj)
    touchMenus()
  end
  removeValue(forms, obj)
  touchForms()
end

Impl.object = {
  getclassname = function(self) return className(D[self]) end,
  destroy = destroy,
  free = destroy,
  fieldaddress = function() return 0 end,
  methodaddress = function() return 0 end,
  methodname = function() return "" end,
  inheritsfrom = function(self, name) return inherits(self, lower(tostring(name)):gsub("^t", "")) end,
}
Get.object = {
  classname = function(self, d) return className(d) end,
}

Get.component = {
  componentcount = function(self, d) return #d.components end,
  owner = function(self, d) return d.owner end,
}
Indexed.component = {
  component = {
    get = function(self, d, i) return d.components[(tonumber(i) or -1) + 1] end,
    len = function(self, d) return #d.components end,
  },
  componentbyname = {
    get = function(self, d, name) return findOwned(self, lower(tostring(name)), false) end,
  },
}
Impl.component = {
  getcomponentcount = function(self) return #D[self].components end,
  getcomponent = function(self, i) return D[self].components[(tonumber(i) or -1) + 1] end,
  findcomponentbyname = function(self, name) return findOwned(self, lower(tostring(name or "")), false) end,
  getowner = function(self) return D[self].owner end,
}
Defaults.component = { name = "", tag = 0 }

local function controlText(self, d)
  local text = d.props.caption
  if text == nil then
    text = d.props.text
  end
  return text or ""
end

Get.control = {
  parent = function(self, d) return d.parent end,
  caption = controlText,
  text = controlText,
  clientwidth = function(self, d) return d.hostW or d.props.width or 100 end,
  clientheight = function(self, d) return d.hostH or d.props.height or 100 end,
  boundsrect = function(self, d)
    local left, top = d.props.left or 0, d.props.top or 0
    return { Left = left, Top = top, Right = left + (d.props.width or 100), Bottom = top + (d.props.height or 100) }
  end,
}
Set.control = {
  parent = function(self, d, value) setParent(self, value) end,
  caption = function(self, d, value) d.props.caption = value; d.props.text = nil end,
  text = function(self, d, value) d.props.caption = value; d.props.text = nil end,
}
Defaults.control = {
  visible = true, enabled = true, left = 0, top = 0, width = 100, height = 100, color = 0x20000000, align = 0, autosize = false,
  showhint = false, hint = "", cursor = 0, parentcolor = false, parentfont = true, parentshowhint = true, doublebuffered = false,
  borderwidth = 0, bevelouter = 0, bevelinner = 0, bevelwidth = 1, bevelcolor = 0x20000000, borderstyle = 0, transparent = false,
  alignment = 0, layout = 0, wordwrap = false, anchors = "[akTop,akLeft]", tabstop = true, taborder = 0, dragmode = 0,
  constraints = function(self) return new("constraints", self) end,
  borderspacing = function(self) return new("borderspacing", self) end,
  font = function(self) return new("font", self) end,
}
Impl.control = {
  getposition = function(self) local d = D[self] return d.props.left or 0, d.props.top or 0 end,
  setposition = function(self, x, y) local d = D[self] d.props.left, d.props.top = x, y if layout.boundsChanged then layout.boundsChanged(self, d) end end,
  getsize = function(self) local d = D[self] return d.props.width or 100, d.props.height or 100 end,
  setsize = function(self, w, h) local d = D[self] d.props.width, d.props.height = w, h if layout.boundsChanged then layout.boundsChanged(self, d) end end,
  doclick = function(self) return call(D[self].props.onclick, self) end,
  click = function(self) return call(D[self].props.onclick, self) end,
  clienttoscreen = function(self, x, y) return x or 0, y or 0 end,
  screentoclient = function(self, x, y) return x or 0, y or 0 end,
  canfocus = function() return true end,
  focused = function() return false end,
  show = function(self) D[self].props.visible = true if layout.changed then layout.changed(self, D[self], "visible") end end,
  hide = function(self) D[self].props.visible = false if layout.changed then layout.changed(self, D[self], "visible") end end,
  setbounds = function(self, l, t, w, h) local d = D[self] local p = d.props p.left, p.top, p.width, p.height = l, t, w, h if layout.boundsChanged then layout.boundsChanged(self, d) end end,
}

Get.wincontrol = {
  controlcount = function(self, d) return #d.controls end,
}
Indexed.wincontrol = {
  control = {
    get = function(self, d, i) return d.controls[(tonumber(i) or -1) + 1] end,
    len = function(self, d) return #d.controls end,
  },
}
Impl.wincontrol = {
  getcontrolcount = function(self) return #D[self].controls end,
  getcontrol = function(self, i) return D[self].controls[(tonumber(i) or -1) + 1] end,
  getcontrolatpos = function() return nil end,
}
Defaults.wincontrol = {
  canvas = function(self) return new("canvas", self) end,
  handle = 0,
}

local function splitLines(text)
  local items = {}
  text = tostring(text or "")
  if text == "" then
    return items
  end
  local position = 1
  while position <= #text do
    local a, b = text:find("\r?\n", position)
    if not a then
      a, b = text:find("\r", position)
    end
    if not a then
      items[#items + 1] = text:sub(position)
      break
    end
    items[#items + 1] = text:sub(position, a - 1)
    position = b + 1
  end
  return items
end

local function stringsChanged(self, d)
  touchForms()
  if d.onChange then
    d.onChange(self, d)
  end
  call(d.props.onchange, self)
end

Init.strings = function(self, d)
  d.items = {}
  d.data = {}
end
Get.strings = {
  count = function(self, d) return #d.items end,
  text = function(self, d)
    if #d.items == 0 then
      return ""
    end
    return table.concat(d.items, "\r\n") .. "\r\n"
  end,
  commatext = function(self, d) return table.concat(d.items, ",") end,
  linebreak = function() return "\r\n" end,
}
Set.strings = {
  text = function(self, d, value)
    d.items = splitLines(value)
    d.data = {}
    stringsChanged(self, d)
  end,
  commatext = function(self, d, value)
    d.items = {}
    for part in tostring(value or ""):gmatch("[^,]+") do
      d.items[#d.items + 1] = part
    end
    stringsChanged(self, d)
  end,
}
IndexGet.strings = function(self, d, i) return d.items[i + 1] end
IndexSet.strings = function(self, d, i, v)
  if i >= 0 and i < #d.items then
    d.items[i + 1] = tostring(v)
    stringsChanged(self, d)
  end
end
Indexed.strings = {
  string = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, set = function(self, d, i, v) IndexSet.strings(self, d, i, v) end, len = function(self, d) return #d.items end },
  strings = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, set = function(self, d, i, v) IndexSet.strings(self, d, i, v) end, len = function(self, d) return #d.items end },
  data = { get = function(self, d, i) return d.data[(tonumber(i) or -1) + 1] end, set = function(self, d, i, v) d.data[i + 1] = v end },
  objects = { get = function(self, d, i) return d.data[(tonumber(i) or -1) + 1] end, set = function(self, d, i, v) d.data[i + 1] = v end },
  values = {
    get = function(self, d, name)
      for _, line in ipairs(d.items) do
        local k, v = line:match("^([^=]*)=(.*)$")
        if k == name then
          return v
        end
      end
      return ""
    end,
    set = function(self, d, name, value)
      for i, line in ipairs(d.items) do
        if line:match("^([^=]*)=") == name then
          d.items[i] = name .. "=" .. tostring(value)
          return
        end
      end
      d.items[#d.items + 1] = name .. "=" .. tostring(value)
    end,
  },
  names = { get = function(self, d, i) local line = d.items[(tonumber(i) or -1) + 1] return line and line:match("^([^=]*)=") or "" end },
}
Impl.strings = {
  add = function(self, text)
    local d = D[self]
    d.items[#d.items + 1] = tostring(text == nil and "" or text)
    stringsChanged(self, d)
    return #d.items - 1
  end,
  addobject = function(self, text, data)
    local d = D[self]
    d.items[#d.items + 1] = tostring(text == nil and "" or text)
    d.data[#d.items] = data
    stringsChanged(self, d)
    return #d.items - 1
  end,
  addtext = function(self, text)
    local d = D[self]
    for _, line in ipairs(splitLines(text)) do
      d.items[#d.items + 1] = line
    end
    stringsChanged(self, d)
  end,
  addstrings = function(self, other)
    local d = D[self]
    local source = D[other]
    if source and source.items then
      for _, line in ipairs(source.items) do
        d.items[#d.items + 1] = line
      end
    elseif type(other) == "table" then
      for _, line in ipairs(other) do
        d.items[#d.items + 1] = tostring(line)
      end
    end
    stringsChanged(self, d)
  end,
  assign = function(self, other)
    local d = D[self]
    local source = D[other]
    d.items = {}
    d.data = {}
    if source and source.items then
      for i, line in ipairs(source.items) do
        d.items[i] = line
      end
    end
    stringsChanged(self, d)
  end,
  clear = function(self)
    local d = D[self]
    d.items = {}
    d.data = {}
    stringsChanged(self, d)
  end,
  delete = function(self, index)
    local d = D[self]
    index = tonumber(index) or -1
    if index >= 0 and index < #d.items then
      table.remove(d.items, index + 1)
      table.remove(d.data, index + 1)
      stringsChanged(self, d)
    end
  end,
  insert = function(self, index, text)
    local d = D[self]
    index = math.max(0, math.min(tonumber(index) or 0, #d.items))
    table.insert(d.items, index + 1, tostring(text))
    stringsChanged(self, d)
  end,
  remove = function(self, text)
    local d = D[self]
    for i, line in ipairs(d.items) do
      if line == text then
        table.remove(d.items, i)
        stringsChanged(self, d)
        return i - 1
      end
    end
    return -1
  end,
  indexof = function(self, text)
    for i, line in ipairs(D[self].items) do
      if line == text then
        return i - 1
      end
    end
    return -1
  end,
  indexofname = function(self, name)
    for i, line in ipairs(D[self].items) do
      if line:match("^([^=]*)=") == name then
        return i - 1
      end
    end
    return -1
  end,
  exchange = function(self, a, b)
    local items = D[self].items
    items[a + 1], items[b + 1] = items[b + 1], items[a + 1]
  end,
  sort = function(self) table.sort(D[self].items) end,
  gettext = function(self) return self.Text end,
  settext = function(self, text) self.Text = text end,
  getcount = function(self) return #D[self].items end,
  getstring = function(self, i) return D[self].items[(tonumber(i) or -1) + 1] end,
  setstring = function(self, i, v) IndexSet.strings(self, D[self], i, v) end,
  beginupdate = noop,
  endupdate = noop,
  loadfromfile = function(self, path)
    local file = io.open(path, "rb")
    if not file then
      error("Unable to open file \"" .. tostring(path) .. "\"")
    end
    local text = file:read("a")
    file:close()
    if text:sub(1, 3) == "\239\187\191" then
      text = text:sub(4)
    end
    self.Text = text
  end,
)MLUA"),
            std::string_view(R"MLUA(  savetofile = function(self, path)
    local file = io.open(path, "wb")
    if not file then
      error("Unable to create file \"" .. tostring(path) .. "\"")
    end
    file:write(self.Text)
    file:close()
  end,
}
ExtraParents.stringlist = "strings"
Defaults.stringlist = { sorted = false, duplicates = 0, casesensitive = false }

Defaults.memo = {
  lines = function(self, d)
    local lines = new("stringlist", self)
    D[lines].onChange = function()
      call(D[self].props.onchange, self)
    end
    return lines
  end,
  scrollbars = 0,
  readonly = false,
  wordwrap = true,
  wantreturns = true,
  wanttabs = false,
}
Get.memo = {
  text = function(self, d)
    local text = self.Lines.Text
    if text:sub(-2) == "\r\n" then
      text = text:sub(1, -3)
    end
    return text
  end,
  caption = function(self, d) return Get.memo.text(self, d) end,
}
Set.memo = {
  text = function(self, d, value) self.Lines.Text = value end,
  caption = function(self, d, value) self.Lines.Text = value end,
}
Impl.memo = {
  append = function(self, text) self.Lines.add(text) end,
  clear = function(self) self.Lines.clear() end,
  clearselection = noop,
  selectall = noop,
  copytoclipboard = noop,
  pastefromclipboard = noop,
}

Get.edit = {
  text = function(self, d) return d.props.text or "" end,
  caption = function(self, d) return d.props.text or "" end,
}
Set.edit = {
  text = function(self, d, value)
    d.props.text = value == nil and "" or tostring(value)
    call(d.props.onchange, self)
  end,
  caption = function(self, d, value) d.props.text = value == nil and "" or tostring(value) end,
}
Defaults.edit = { selstart = 0, sellength = 0, seltext = "", maxlength = 0, passwordchar = "", readonly = false, numbersonly = false, charcase = 0, texthint = "" }
Impl.edit = {
  clear = function(self) self.Text = "" end,
  selectall = noop,
  copytoclipboard = noop,
  cuttoclipboard = noop,
  pastefromclipboard = noop,
  clearselection = noop,
}

local function listText(self, d)
  local index = d.props.itemindex or -1
  local items = self.Items
  if index >= 0 and items[index] then
    return items[index]
  end
  return d.props.text or ""
end

Defaults.combobox = { items = function(self) return new("stringlist", self) end, itemindex = -1, style = 0, dropdowncount = 8, readonly = false }
Get.combobox = { text = listText, caption = listText }
Set.combobox = {
  text = function(self, d, value)
    d.props.text = tostring(value)
    local found = self.Items.indexOf(tostring(value))
    if found >= 0 then
      d.props.itemindex = found
    end
  end,
  itemindex = function(self, d, value)
    d.props.itemindex = tonumber(value) or -1
    d.props.text = nil
    call(d.props.onchange, self)
    call(d.props.onselect, self)
  end,
}
Impl.combobox = { clear = function(self) self.Items.clear() D[self].props.itemindex = -1 D[self].props.text = "" end }

Defaults.listbox = { items = function(self) return new("stringlist", self) end, itemindex = -1, multiselect = false }
Get.listbox = { text = listText, selcount = function(self, d) return (d.props.itemindex or -1) >= 0 and 1 or 0 end }
Indexed.listbox = {
  selected = {
    get = function(self, d, i) return (d.props.itemindex or -1) == i end,
    set = function(self, d, i, v) if v then d.props.itemindex = i end end,
  },
}
Impl.listbox = { clear = function(self) self.Items.clear() D[self].props.itemindex = -1 end, getitemindex = function(self) return D[self].props.itemindex or -1 end, setitemindex = function(self, v) D[self].props.itemindex = v end }

Defaults.radiogroup = { items = function(self) return new("stringlist", self) end, itemindex = -1, columns = 1 }
Defaults.checkbox = { checked = false, state = 0, allowgrayed = false }
local function luaShowOnPrint()
  return manifold and manifold.output.getShowOnPrint() or false
end
Get.checkbox = {
  checked = function(self, d)
    if d.showOnPrint then
      return luaShowOnPrint()
    end
    return false
  end,
  state = function(self, d)
    if d.showOnPrint then
      return luaShowOnPrint() and 1 or 0
    end
    return 0
  end,
}
Set.checkbox = {
  checked = function(self, d, value)
    if d.showOnPrint then
      local before = luaShowOnPrint()
      if manifold then
        manifold.output.setShowOnPrint(value and true or false)
      end
      if before ~= (value and true or false) then
        call(d.props.onchange, self)
      end
      return
    end
    local changed = (d.props.checked or false) ~= (value and true or false)
    d.props.checked = value and true or false
    d.props.state = value and 1 or 0
    if changed then
      call(d.props.onchange, self)
    end
  end,
  state = function(self, d, value)
    if d.showOnPrint then
      Set.checkbox.checked(self, d, value == 1 or value == "cbChecked")
      return
    end
    d.props.state = value
    d.props.checked = value == 1 or value == "cbChecked"
  end,
}
Defaults.button = { modalresult = 0, default = false, cancel = false }
Defaults.progressbar = { min = 0, max = 100, position = 0, step = 10 }
Impl.progressbar = { stepit = function(self) local d = D[self] d.props.position = (d.props.position or 0) + (d.props.step or 10) end, stepby = function(self, n) local d = D[self] d.props.position = (d.props.position or 0) + (n or 0) end }
Defaults.trackbar = { min = 0, max = 10, position = 0 }
Defaults.image = { picture = function(self) return new("picture", self) end, stretch = false, proportional = false, center = false, transparent = false }
Get.image = { canvas = function(self, d) return self.Picture.Bitmap.Canvas end }
Defaults.paintbox = { canvas = function(self) return new("canvas", self) end }
Impl.paintbox = { repaint = function(self) end }
Defaults.phantom = { canvas = function(self) return new("canvas", self) end, items = function(self) return new("stringlist", self) end }
Defaults.splitter = { resizestyle = 0, minsize = 30 }
Defaults.groupbox = {}
Defaults.panel = { bevelouter = 2, fullrepaint = true }

Defaults.font = { name = "Segoe UI", size = 9, height = -12, color = 0x20000000, style = "[]", charset = 1, quality = 0, pitch = 0, orientation = 0 }
Impl.font = { assign = function(self, other) local source = D[other] if source then for k, v in pairs(source.props) do D[self].props[k] = v end end end, isequal = function() return false end }
Defaults.brush = { color = 0xFFFFFF, style = 0 }
Defaults.gutter = { color = 0x20000000, width = 30, visible = true, autosize = true }
Defaults.point = { x = 0, y = 0 }
Defaults.borderspacing = { around = 0, left = 0, top = 0, right = 0, bottom = 0, innerborder = 0, cellalignhorizontal = 0, cellalignvertical = 0 }
Defaults.constraints = { minwidth = 0, minheight = 0, maxwidth = 0, maxheight = 0 }
Defaults.pen = { color = 0, width = 1, style = 0, mode = 0 }
Defaults.canvas = {
  pen = function(self) return new("pen", self) end,
  brush = function(self) return new("brush", self) end,
  font = function(self) return new("font", self) end,
  penposition = 0,
}
Get.canvas = {
  width = function(self, d) local owner = d.owner and D[d.owner] return owner and owner.props.width or 100 end,
  height = function(self, d) local owner = d.owner and D[d.owner] return owner and owner.props.height or 100 end,
}
local function canvasFont(self)
  local font = self.Font
  local size = tonumber(font.Size) or 9
  if size <= 0 then
    size = 9
  end
  local name = lower(tostring(font.Name or ""))
  local mono = name:find("consolas") ~= nil or name:find("courier") ~= nil or name:find("mono") ~= nil
  return size, lower(tostring(font.Style or "")):find("fsbold") ~= nil, mono
end
local function textWidth(self, text)
  local size, bold, mono = canvasFont(self)
  local native = rawget(_G, "__manifoldTextExtent")
  if native then
    local w = native(tostring(text or ""), size, bold, mono)
    if w then
      return w
    end
  end
  return math.floor(#tostring(text or "") * size * 0.75 + 0.5)
end
local function textHeight(self, text)
  local size, bold, mono = canvasFont(self)
  local native = rawget(_G, "__manifoldTextExtent")
  if native then
    local _, h = native(tostring(text or "Xg"), size, bold, mono)
    if h then
      return h
    end
  end
  return math.floor(size * 1.6 + 0.5)
end
Impl.canvas = {
  textwidth = textWidth,
  gettextwidth = textWidth,
  textheight = textHeight,
  gettextheight = textHeight,
  textextent = function(self, text) return textWidth(self, text), textHeight(self, text) end,
  getpixel = function() return 0 end,
  getpenposition = function() return 0, 0 end,
  handleallocated = function() return true end,
}
Defaults.graphic = { width = 0, height = 0, transparent = false, empty = true, modified = false }
Defaults.bitmap = { canvas = function(self) return new("canvas", self) end, pixelformat = 0, transparentcolor = 0x20000000, transparentmode = 0, handle = 0 }
Impl.bitmap = {
  setsize = function(self, w, h) local p = D[self].props p.width, p.height = w, h end,
  loadfromfile = function(self) D[self].props.empty = false end,
  loadfromstream = function(self) D[self].props.empty = false end,
  savetofile = noop,
  savetostream = noop,
  assign = function(self, other) local source = D[other] if source then D[self].props.width = source.props.width D[self].props.height = source.props.height end end,
  clear = noop,
  freeimage = noop,
}
Defaults.picture = {
  bitmap = function(self) return new("bitmap", self) end,
  png = function(self) return new("png", self) end,
  portablenetworkgraphic = function(self) return new("png", self) end,
  jpeg = function(self) return new("jpeg", self) end,
  icon = function(self) return new("icon", self) end,
  graphic = function(self) return new("bitmap", self) end,
}
Get.picture = {
  width = function(self) return self.Graphic.Width end,
  height = function(self) return self.Graphic.Height end,
}
Impl.picture = { loadfromfile = noop, loadfromstream = noop, savetofile = noop, assign = noop, clear = noop }

Init.imagelist = function(self, d) d.images = {} end
Defaults.imagelist = { width = 16, height = 16, masked = false, bkcolor = 0x1FFFFFFF }
Get.imagelist = { count = function(self, d) return #d.images end }
Impl.imagelist = {
  add = function(self, image) local d = D[self] d.images[#d.images + 1] = image or true return #d.images - 1 end,
  addmasked = function(self, image) local d = D[self] d.images[#d.images + 1] = image or true return #d.images - 1 end,
  addicon = function(self, image) local d = D[self] d.images[#d.images + 1] = image or true return #d.images - 1 end,
  insert = function(self, index, image) table.insert(D[self].images, (index or 0) + 1, image or true) end,
  delete = function(self, index) table.remove(D[self].images, (index or 0) + 1) end,
  clear = function(self) D[self].images = {} end,
  getbitmap = noop,
  draw = noop,
  replace = noop,
}

Init.menuitem = function(self, d) d.items = {} end
Defaults.menuitem = { caption = "", shortcut = "", imageindex = -1, checked = false, radioitem = false, autocheck = false, default = false, visible = true, enabled = true, groupindex = 0, hint = "" }
Get.menuitem = {
  count = function(self, d) return #d.items end,
  parent = function(self, d) return d.parentItem end,
  menuindex = function(self, d)
    local parent = d.parentItem and D[d.parentItem]
    if not parent then
      return -1
    end
    for i, item in ipairs(parent.items) do
      if rawequal(item, self) then
        return i - 1
      end
    end
    return -1
  end,
  menu = function(self, d)
    local current = self
    while D[current] and D[current].parentItem do
      current = D[current].parentItem
    end
    return D[current] and D[current].menu
  end,
}
Set.menuitem = {
  menuindex = function(self, d, value)
    local parent = d.parentItem and D[d.parentItem]
    if not parent then
      return
    end
    removeValue(parent.items, self)
    table.insert(parent.items, math.max(1, math.min((tonumber(value) or 0) + 1, #parent.items + 1)), self)
  end,
}
IndexGet.menuitem = function(self, d, i) return d.items[i + 1] end
)MLUA"),
            std::string_view(R"MLUA(Indexed.menuitem = {
  item = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.items end },
  items = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.items end },
}
local function attachItem(parent, item, index)
  local d = D[parent]
  local child = D[item]
  if not child then
    return
  end
  touchMenus()
  if child.parentItem and D[child.parentItem] then
    removeValue(D[child.parentItem].items, item)
  end
  child.parentItem = parent
  if index then
    table.insert(d.items, math.max(1, math.min(index + 1, #d.items + 1)), item)
  else
    d.items[#d.items + 1] = item
  end
end
Impl.menuitem = {
  add = function(self, item) attachItem(self, item) end,
  insert = function(self, index, item) attachItem(self, item, tonumber(index) or 0) end,
  delete = function(self, index)
    local d = D[self]
    local item = d.items[(tonumber(index) or -1) + 1]
    if item then
      table.remove(d.items, index + 1)
      D[item].parentItem = nil
      touchMenus()
    end
  end,
  remove = function(self, item)
    removeValue(D[self].items, item)
    if D[item] then
      D[item].parentItem = nil
    end
    touchMenus()
  end,
  clear = function(self)
    for _, item in ipairs(D[self].items) do
      D[item].parentItem = nil
    end
    D[self].items = {}
    touchMenus()
  end,
  indexof = function(self, item)
    for i, child in ipairs(D[self].items) do
      if rawequal(child, item) then
        return i - 1
      end
    end
    return -1
  end,
  find = function(self, caption)
    local wanted = tostring(caption or ""):gsub("&", "")
    for _, child in ipairs(D[self].items) do
      if tostring(child.Caption or ""):gsub("&", "") == wanted then
        return child
      end
    end
    return nil
  end,
  getitem = function(self, i) return D[self].items[(tonumber(i) or -1) + 1] end,
  getcount = function(self) return #D[self].items end,
  doclick = function(self) return call(D[self].props.onclick, self) end,
  click = function(self) return call(D[self].props.onclick, self) end,
}
Defaults.menu = {
  items = function(self)
    local root = new("menuitem", self)
    D[root].menu = self
    return root
  end,
  images = nil,
}
Get.menu = { images = function(self, d) return d.props.images end }
Impl.menu = { getitems = function(self) return self.Items end }
Impl.popupmenu = { popup = noop, close = noop }

Init.listitems = function(self, d) d.items = {} end
local function newListItem(owner)
  local item = new("listitem", owner)
  D[item].listOwner = owner
  return item
end
Get.listitems = { count = function(self, d) return #d.items end }
Set.listitems = {
  count = function(self, d, value)
    value = tonumber(value) or 0
    while #d.items < value do
      d.items[#d.items + 1] = newListItem(self)
    end
    while #d.items > value do
      table.remove(d.items)
    end
  end,
}
IndexGet.listitems = function(self, d, i) return d.items[i + 1] end
Indexed.listitems = { item = { get = function(self, d, i) return d.items[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.items end } }
Impl.listitems = {
  add = function(self) touchForms() local d = D[self] local item = newListItem(self) d.items[#d.items + 1] = item return item end,
  insert = function(self, index) touchForms() local d = D[self] local item = newListItem(self) table.insert(d.items, math.max(1, math.min((index or 0) + 1, #d.items + 1)), item) return item end,
  clear = function(self) touchForms() D[self].items = {} end,
  delete = function(self, index) touchForms() table.remove(D[self].items, (tonumber(index) or -1) + 1) end,
  beginupdate = noop,
  endupdate = noop,
  getitem = function(self, i) return D[self].items[(tonumber(i) or -1) + 1] end,
  getcount = function(self) return #D[self].items end,
}
Defaults.listitem = {
  caption = "",
  subitems = function(self) return new("stringlist", self) end,
  imageindex = -1,
  stateindex = -1,
  selected = false,
  checked = false,
  data = 0,
  focused = false,
}
Get.listitem = {
  index = function(self, d)
    local owner = d.listOwner and D[d.listOwner]
    if owner then
      for i, item in ipairs(owner.items) do
        if rawequal(item, self) then
          return i - 1
        end
      end
    end
    return -1
  end,
  owner = function(self, d) return d.listOwner end,
}
Impl.listitem = {
  delete = function(self) local d = D[self] if d.listOwner then removeValue(D[d.listOwner].items, self) end end,
  makevisible = noop,
  displayrect = function() return { Left = 0, Top = 0, Right = 0, Bottom = 0 } end,
}

Init.listcolumns = function(self, d) d.items = {} end
Get.listcolumns = { count = function(self, d) return #d.items end }
IndexGet.listcolumns = function(self, d, i) return d.items[i + 1] end
Impl.listcolumns = {
  add = function(self) local d = D[self] local column = new("listcolumn", self) d.items[#d.items + 1] = column return column end,
  clear = function(self) D[self].items = {} end,
  delete = function(self, index) table.remove(D[self].items, (tonumber(index) or -1) + 1) end,
  getcount = function(self) return #D[self].items end,
  getcolumn = function(self, i) return D[self].items[(tonumber(i) or -1) + 1] end,
  beginupdate = noop,
  endupdate = noop,
}
Defaults.listcolumn = { caption = "", width = 50, autosize = false, alignment = 0, visible = true, minwidth = 0, maxwidth = 0, imageindex = -1, tag = 0 }

Defaults.listview = {
  items = function(self) return new("listitems", self) end,
  columns = function(self) return new("listcolumns", self) end,
  itemindex = -1,
  viewstyle = 0,
  rowselect = false,
  readonly = false,
  multiselect = false,
  checkboxes = false,
  hideselection = true,
  gridlines = false,
  ownerdata = false,
  showcolumnheaders = true,
}
Get.listview = {
  selected = function(self, d)
    local index = d.props.itemindex or -1
    return index >= 0 and self.Items[index] or nil
  end,
  selcount = function(self, d) return (d.props.itemindex or -1) >= 0 and 1 or 0 end,
}
Set.listview = {
  selected = function(self, d, item)
    d.props.itemindex = item and item.Index or -1
  end,
}
Impl.listview = {
  clear = function(self) touchForms() self.Items.clear() D[self].props.itemindex = -1 end,
  getitemat = function() return nil end,
  beginupdate = noop,
  endupdate = noop,
}

Init.treenodes = function(self, d) d.nodes = {} d.roots = {} end
local function newNode(nodes, parent, text)
  touchForms()
  local node = new("treenode", nodes)
  local nd = D[node]
  nd.tree = nodes
  nd.children = {}
  nd.parentNode = parent
  nd.props.text = tostring(text or "")
  local list = parent and D[parent].children or D[nodes].roots
  list[#list + 1] = node
  local all = D[nodes].nodes
  all[#all + 1] = node
  return node
end
local function removeNode(node)
  local nd = D[node]
  if not nd then
    return
  end
  touchForms()
  local tree = nd.tree and D[nd.tree] and D[nd.tree].owner
  if tree and D[tree] and rawequal(D[tree].props.selected, node) then
    D[tree].props.selected = nil
  end
  for i = #nd.children, 1, -1 do
    removeNode(nd.children[i])
  end
  local tree = D[nd.tree]
  removeValue(tree.nodes, node)
  removeValue(nd.parentNode and D[nd.parentNode].children or tree.roots, node)
end
Get.treenodes = { count = function(self, d) return #d.nodes end }
IndexGet.treenodes = function(self, d, i) return d.nodes[i + 1] end
Indexed.treenodes = { item = { get = function(self, d, i) return d.nodes[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.nodes end } }
local function nodeArguments(first, second)
  if isObject(first) then
    return first, second
  end
  if first ~= nil and second == nil then
    return nil, first
  end
  return nil, second
end
local function insertNode(nodes, sibling, text, after)
  local node = newNode(nodes, sibling and D[sibling] and D[sibling].parentNode or nil, text)
  if sibling and D[sibling] then
    local nd = D[node]
    local list = nd.parentNode and D[nd.parentNode].children or D[nodes].roots
    removeValue(list, node)
    local at = #list + 1
    for i, other in ipairs(list) do
      if rawequal(other, sibling) then
        at = after and i + 1 or i
        break
      end
    end
    table.insert(list, at, node)
  end
  return node
end
Impl.treenodes = {
  add = function(self, a, b)
    local sibling, text = nodeArguments(a, b)
    local parent = sibling and D[sibling] and D[sibling].parentNode or nil
    return newNode(self, parent, text)
  end,
  addchild = function(self, parent, text) return newNode(self, isObject(parent) and parent or nil, text) end,
  addfirst = function(self, a, b)
    local sibling, text = nodeArguments(a, b)
    return Impl.treenodes.add(self, sibling, text)
  end,
  addchildfirst = function(self, parent, text)
    local node = newNode(self, isObject(parent) and parent or nil, text)
    local nd = D[node]
    local list = nd.parentNode and D[nd.parentNode].children or D[self].roots
    removeValue(list, node)
    table.insert(list, 1, node)
    return node
  end,
  insert = function(self, sibling, text) return insertNode(self, isObject(sibling) and sibling or nil, text, false) end,
  insertbehind = function(self, sibling, text) return insertNode(self, isObject(sibling) and sibling or nil, text, true) end,
  clear = function(self)
    local d = D[self]
    d.nodes = {}
    d.roots = {}
    local tree = d.owner
    if tree and D[tree] then
      D[tree].props.selected = nil
    end
    touchForms()
  end,
  delete = function(self, node) removeNode(node) end,
  getfirstnode = function(self) return D[self].roots[1] end,
  getcount = function(self) return #D[self].nodes end,
  getitem = function(self, i) return D[self].nodes[(tonumber(i) or -1) + 1] end,
  beginupdate = noop,
  endupdate = noop,
}
Defaults.treenode = { text = "", data = 0, imageindex = -1, selectedindex = -1, stateindex = -1, expanded = false, selected = false }
Get.treenode = {
  count = function(self, d) return #d.children end,
  parent = function(self, d) return d.parentNode end,
  haschildren = function(self, d) return #d.children > 0 or d.props.haschildren == true end,
  level = function(self, d)
    local level, current = 0, d.parentNode
    while current do
      level = level + 1
      current = D[current].parentNode
    end
    return level
  end,
  index = function(self, d)
    local list = d.parentNode and D[d.parentNode].children or D[d.tree].roots
    for i, node in ipairs(list) do
      if rawequal(node, self) then
        return i - 1
      end
    end
    return -1
  end,
  absoluteindex = function(self, d)
    for i, node in ipairs(D[d.tree].nodes) do
      if rawequal(node, self) then
        return i - 1
      end
    end
    return -1
  end,
  owner = function(self, d) return d.tree end,
}
IndexGet.treenode = function(self, d, i) return d.children[i + 1] end
Indexed.treenode = {
  items = { get = function(self, d, i) return d.children[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.children end },
  item = { get = function(self, d, i) return d.children[(tonumber(i) or -1) + 1] end, len = function(self, d) return #d.children end },
}
local function sibling(self, delta)
  local d = D[self]
  local list = d.parentNode and D[d.parentNode].children or D[d.tree].roots
  for i, node in ipairs(list) do
    if rawequal(node, self) then
      return list[i + delta]
    end
  end
end
Impl.treenode = {
  add = function(self, text) return newNode(D[self].tree, self, text) end,
  addchild = function(self, text) return newNode(D[self].tree, self, text) end,
  delete = function(self) removeNode(self) end,
  deletechildren = function(self) local d = D[self] for i = #d.children, 1, -1 do removeNode(d.children[i]) end end,
  expand = function(self, recursive)
    local function open(node, deep)
      D[node].props.expanded = true
      if deep then
        for _, child in ipairs(D[node].children) do
          open(child, true)
        end
      end
    end
)MLUA"),
            std::string_view(R"MLUA(    open(self, recursive ~= false)
    touchForms()
  end,
  collapse = function(self, recursive)
    local function close(node, deep)
      D[node].props.expanded = false
      if deep then
        for _, child in ipairs(D[node].children) do
          close(child, true)
        end
      end
    end
    close(self, recursive ~= false)
    touchForms()
  end,
  makevisible = noop,
  getfirstchild = function(self) return D[self].children[1] end,
  getlastchild = function(self) local c = D[self].children return c[#c] end,
  getnextsibling = function(self) return sibling(self, 1) end,
  getprevsibling = function(self) return sibling(self, -1) end,
  getnext = function(self) return D[self].children[1] or sibling(self, 1) end,
  displayrect = function() return { Left = 0, Top = 0, Right = 0, Bottom = 0 } end,
  getitem = function(self, i) return D[self].children[(tonumber(i) or -1) + 1] end,
  getcount = function(self) return #D[self].children end,
  getparent = function(self) return D[self].parentNode end,
}
Defaults.treeview = { items = function(self) return new("treenodes", self) end, readonly = false, multiselect = false, rightclickselect = false, hideselection = true, showroot = true, showlines = true }
Impl.treeview = {
  clear = function(self) self.Items.clear() D[self].props.selected = nil end,
  fullexpand = noop,
  fullcollapse = noop,
  getnodeat = function() return nil end,
  beginupdate = noop,
  endupdate = noop,
}

local function addForm(form)
  forms[#forms + 1] = form
  for _, entry in pairs(formNotifications) do
    call(entry, form)
  end
end

Defaults.form = {
  visible = false, caption = "", position = 0, borderstyle = 2, formstyle = 0, windowstate = 0, modalresult = 0, alphablend = false,
  alphablendvalue = 255, keypreview = false, showintaskbar = 0, designtimeppi = 96, width = 400, height = 300, popupparent = nil,
}
Set.form = {
  visible = function(self, d, value)
    d.props.visible = value and true or false
    touchForms()
  end,
}
Impl.form = {
  show = function(self)
    local d = D[self]
    if d.props.visible then
      d.raise = (d.raise or 0) + 1
    end
    d.props.visible = true
    touchForms()
    call(d.props.onshow, self)
  end,
  hide = function(self)
    D[self].props.visible = false
    touchForms()
  end,
  close = function(self)
    local d = D[self]
    local action = call(d.props.onclose, self)
    d.props.visible = false
    touchForms()
    if action == caFree then
      destroy(self)
    end
  end,
  showmodal = function(self)
    local d = D[self]
    call(d.props.onshow, self)
    if __manifoldHeadless then
      return d.props.modalresult ~= 0 and d.props.modalresult or mrCancel
    end
    return d.props.modalresult ~= 0 and d.props.modalresult or mrCancel
  end,
  centerscreen = noop,
  bringtofront = function(self)
    local d = D[self]
    d.raise = (d.raise or 0) + 1
    touchForms()
  end,
  sendtoback = noop,
  dragnow = noop,
  setfocus = noop,
  repaint = noop,
  update = noop,
  invalidate = noop,
  refresh = noop,
  loadformposition = function() return false end,
  saveformposition = noop,
  isforegroundwindow = function() return false end,
  registerclosecallback = noop,
  registercreatecallback = noop,
  registerfirstshowcallback = noop,
  fixdpi = noop,
  printtorasterimage = noop,
}

Defaults.filedialog = { filename = "", filter = "", filterindex = 1, initialdir = "", title = "", defaultext = "", options = "[]", files = function(self) return new("stringlist", self) end }
Impl.filedialog = {
  execute = function(self)
    if __manifoldHeadless then
      return false
    end
    local d = D[self]
    local save = d.class == "savedialog"
    local chosen = __manifoldFileDialog(save, self.Title, self.Filter, self.FileName, self.InitialDir, self.DefaultExt)
    if not chosen then
      return false
    end
    d.props.filename = chosen
    self.Files.clear()
    self.Files.add(chosen)
    return true
  end,
}
Impl.selectdirectorydialog = {
  execute = function(self)
    if __manifoldHeadless then
      return false
    end
    local chosen = __manifoldFolderDialog(self.Title ~= "" and self.Title or nil)
    if not chosen then
      return false
    end
    D[self].props.filename = chosen
    return true
  end,
}
Defaults.colordialog = { color = 0, title = "" }
Impl.colordialog = {
  execute = function(self)
    if __manifoldHeadless then
      return false
    end
    local chosen = __manifoldColorDialog(self.Color)
    if not chosen then
      return false
    end
    D[self].props.color = chosen
    return true
  end,
}
Impl.fontdialog = { execute = function() return false end }
Impl.finddialog = { execute = function() return false end, closedialog = noop }
Defaults.fontdialog = { font = function(self) return new("font", self) end }

Defaults.thread = { terminated = false, name = "", freeonterminate = true, finished = false }
Impl.thread = {
  synchronize = function(self, fn, ...) return call(fn, self, ...) end,
  queue = function(self, fn, ...) return call(fn, self, ...) end,
  terminate = function(self) D[self].props.terminated = true end,
  waitfor = noop,
  waittilldone = noop,
  freeonterminate = function(self, value) D[self].props.freeonterminate = value end,
  resume = function(self) D[self].suspended = nil end,
  suspend = noop,
}

Defaults.disassemblerview = { selectedaddress2 = 0, topaddress = 0, showjumplines = true, hidefocusrect = false }
Get.disassemblerview = { selectedaddress = function() return __manifoldSelectedCodeAddress() end }
Set.disassemblerview = { selectedaddress = function(self, d, value) __manifoldSetSelectedCodeAddress(value) end }
Defaults.hexadecimal = { displaytype = 0 }
local function hexView(d, address)
  local f = rawget(_G, "__manifoldSetHexView")
  if f then
    f(address or 0, d.selStart or 0, d.selStop or 0)
  end
end
Get.hexadecimal = {
  address = function() return __manifoldSelectedHexAddress() end,
  topaddress = function() return __manifoldSelectedHexAddress() end,
  selectionstart = function(self, d) return d.selStart or 0 end,
  selectionstop = function(self, d) return d.selStop or 0 end,
}
Set.hexadecimal = {
  address = function(self, d, value)
    d.selStart, d.selStop = nil, nil
    hexView(d, tonumber(value))
  end,
  topaddress = function(self, d, value)
    d.selStart, d.selStop = nil, nil
    hexView(d, tonumber(value))
  end,
  selectionstart = function(self, d, value)
    d.selStart = tonumber(value) or 0
    if (d.selStop or 0) < d.selStart then
      d.selStop = d.selStart
    end
    hexView(d, 0)
  end,
  selectionstop = function(self, d, value)
    d.selStop = tonumber(value) or 0
    if (d.selStart or 0) == 0 or d.selStart > d.selStop then
      d.selStart = d.selStop
    end
    hexView(d, 0)
  end,
}

local function construct(class, owner)
  local obj = new(class, owner)
  if isObject(owner) and inherits(owner, "wincontrol") and inherits(obj, "control") then
    setParent(obj, owner)
  end
  return obj
end

local creators = {
  createLabel = "label", createEdit = "edit", createMemo = "memo", createButton = "button", createCheckBox = "checkbox",
  createToggleBox = "togglebox", createComboBox = "combobox", createListBox = "listbox", createListView = "listview",
  createTreeView = "treeview", createPanel = "panel", createGroupBox = "groupbox", createRadioGroup = "radiogroup",
  createImage = "image", createPaintBox = "paintbox", createSplitter = "splitter", createProgressBar = "progressbar",
  createTrackBar = "trackbar", createScrollBox = "scrollbox", createPageControl = "pagecontrol", createTabSheet = "tabsheet",
  createStatusBar = "statusbar", createSynEdit = "synedit", createCECustomButton = "cecustombutton",
}
for name, class in pairs(creators) do
  if rawget(_G, name) == nil then
    _G[name] = function(owner) return construct(class, owner) end
  end
end

local plain = {
  createMainMenu = "mainmenu", createPopupMenu = "popupmenu", createMenuItem = "menuitem", createImageList = "imagelist",
  createPicture = "picture", createFont = "font", createOpenDialog = "opendialog", createSaveDialog = "savedialog",
  createSelectDirectoryDialog = "selectdirectorydialog", createColorDialog = "colordialog", createFontDialog = "fontdialog",
  createFindDialog = "finddialog",
}
for name, class in pairs(plain) do
  if rawget(_G, name) == nil then
    _G[name] = function(owner)
      local obj = new(class, owner)
      if class == "mainmenu" and isObject(owner) then
        D[owner].props.menu = obj
      end
      return obj
    end
  end
end

createStringlist = createStringlist or function() return new("stringlist") end
createStringList = createStringList or createStringlist

local function graphic(class)
  return function(width, height)
    local obj = new(class)
    D[obj].props.width = tonumber(width) or 0
    D[obj].props.height = tonumber(height) or 0
    return obj
  end
end
createBitmap = createBitmap or graphic("bitmap")
createPNG = createPNG or graphic("png")
createJpeg = createJpeg or graphic("jpeg")
createIcon = createIcon or graphic("icon")

createForm = createForm or function(visible)
  local form = new("cefrm")
  D[form].script = true
  if visible == nil or visible then
    D[form].props.visible = true
  end
  addForm(form)
  touchForms()
  return form
end

local mainForm = new("mainform")
do
  local p = D[mainForm].props
  p.name = "MainForm"
  p.caption = "Cheat Engine 7.5"
  p.width, p.height = 900, 700
  p.visible = true
  p.onprocessopened = function() end
  local menu = new("mainmenu", mainForm)
  D[menu].props.name = "MainMenu1"
  p.menu = menu
  for _, entry in ipairs({ { "miFile", "&File" }, { "miEdit", "&Edit" }, { "miTable", "&Table" }, { "miD3D", "D3D" }, { "miHelp", "&Help" } }) do
    local item = new("menuitem", mainForm)
    D[item].props.name = entry[1]
    D[item].props.caption = entry[2]
    D[item].builtin = true
    menu.Items.add(item)
  end
  local tableMenu = findOwned(mainForm, "mitable", false)
  local fixed = {}
  for _, entry in ipairs({ { "miShowLuaScript", "Show Cheat Table Lua Script" }, { "miAddFile", "Add file" }, { "miTableSeparator", "-" } }) do
    local item = new("menuitem", mainForm)
    D[item].props.name = entry[1]
    D[item].props.caption = entry[2]
    D[item].builtin = true
    tableMenu.add(item)
    fixed[#fixed + 1] = item
  end
  local fileItems = {}
  D[tableMenu].props.onclick = function(sender)
    for _, item in ipairs(fileItems) do
      tableMenu.remove(item)
      destroy(item)
    end
    fileItems = {}
    for _, name in ipairs(__manifoldTableFileNames()) do
      local item = new("menuitem", mainForm)
      D[item].props.caption = name
      D[item].builtin = true
      tableMenu.add(item)
      fileItems[#fileItems + 1] = item
    end
  end
  for _, entry in ipairs({ { "Foundlist3", "listview" }, { "Panel4", "panel" }, { "Panel5", "panel" }, { "lblSigned", "label" }, { "Splitter1", "splitter" }, { "Panel1", "panel" }, { "btnMemoryView", "button" }, { "ProgressBar", "progressbar" } }) do
    local control = construct(entry[2], mainForm)
    D[control].props.name = entry[1]
  end
end
forms[#forms + 1] = mainForm
MainForm = mainForm

getMainForm = getMainForm or function() return mainForm end

local application = new("application")
do
  local p = D[application].props
  p.title = "Cheat Engine 7.5"
  p.mainform = mainForm
  p.icon = new("icon", application)
  p.exename = ""
end
Impl.application = { processmessages = function() if processMessages then return processMessages() end end, terminate = noop, minimize = noop, restore = noop, bringtofront = noop }
getApplication = getApplication or function() return application end

local luaEngineForm
getLuaEngine = getLuaEngine or function()
  if luaEngineForm then
    return luaEngineForm
  end
  luaEngineForm = new("tfrmluaengine")
  local p = D[luaEngineForm].props
  p.name = "frmLuaEngine"
  p.caption = "Lua Engine"
  local function child(name, class, parent)
    local control = new(class, luaEngineForm)
)MLUA"),
            std::string_view(R"MLUA(    setParent(control, parent or luaEngineForm)
    D[control].props.name = name
    return control
  end
  local panel1 = child("Panel1", "panel")
  child("Panel3", "panel")
  child("GroupBox1", "groupbox")
  child("Splitter1", "splitter")
  child("mOutput", "memo", panel1)
  child("mScript", "synedit")
  child("btnExecute", "button")
  local showOnPrint = child("cbShowOnPrint", "checkbox")
  D[showOnPrint].showOnPrint = true
  D[showOnPrint].props.caption = "Show on print"
  local menu = new("mainmenu", luaEngineForm)
  D[luaEngineForm].props.menu = menu
  addForm(luaEngineForm)
  return luaEngineForm
end
createLuaEngine = createLuaEngine or function()
  local form = new("tfrmluaengine")
  local output = construct("memo", form)
  D[output].props.name = "mOutput"
  local script = construct("synedit", form)
  D[script].props.name = "mScript"
  addForm(form)
  return form
end

local memoryView
getMemoryViewForm = getMemoryViewForm or function()
  if memoryView then
    return memoryView
  end
  memoryView = new("memoryview")
  local p = D[memoryView].props
  p.name = "MemoryBrowser"
  p.caption = "Memory Viewer"
  local view = construct("disassemblerview", memoryView)
  D[view].props.name = "DisassemblerView"
  p.disassemblerview = view
  local hex = construct("hexadecimal", memoryView)
  D[hex].props.name = "HexadecimalView"
  p.hexadecimalview = hex
  local menu = new("mainmenu", memoryView)
  p.menu = menu
  local popup = new("popupmenu", memoryView)
  D[popup].props.name = "debuggerpopup"
  local images = new("imagelist", memoryView)
  D[images].props.name = "mvImageList"
  D[popup].props.images = images
  for _, name in ipairs({ "miDebugSetAddress", "Watchmemoryallocations1", "MenuItem14", "InjectDLL1", "CreateThread1", "AutoInject1" }) do
    local item = new("menuitem", memoryView)
    D[item].props.name = name
    D[item].props.caption = name
    D[item].builtin = true
    menu.Items.add(item)
  end
  addForm(memoryView)
  return memoryView
end

Get.memoryview = {
  visible = function()
    local f = rawget(_G, "__manifoldMemoryViewVisible")
    return f and f() or false
  end,
}
Set.memoryview = {
  visible = function(self, d, value)
    if value then
      local f = rawget(_G, "__manifoldShowMemoryView")
      if f then
        f()
      end
    end
  end,
}
Impl.memoryview = {
  show = function(self)
    local f = rawget(_G, "__manifoldShowMemoryView")
    if f then
      f()
    end
    call(D[self].props.onshow, self)
  end,
  bringtofront = function()
    local f = rawget(_G, "__manifoldShowMemoryView")
    if f then
      f()
    end
  end,
  hide = noop,
  close = noop,
}

local uids = setmetatable({}, { __mode = "v" })
local nextUid = 0

local function uidOf(obj)
  local d = D[obj]
  if not d.uid then
    nextUid = nextUid + 1
    d.uid = nextUid
    uids[nextUid] = obj
  end
  return d.uid
end

local function live(id)
  local obj = uids[tonumber(id) or -1]
  local d = obj and D[obj]
  if d and not d.destroyed then
    return obj, d
  end
  return nil
end

local function plainCaption(text)
  return (tostring(text or ""):gsub("&(.)", "%1"))
end

local function shortcutText(value)
  if type(value) == "number" then
    if value == 0 then
      return ""
    end
    local f = rawget(_G, "shortCutToText")
    if f then
      local ok, text = pcall(f, value)
      if ok and type(text) == "string" then
        return text
      end
    end
    return ""
  end
  return tostring(value or "")
end

local function menuEntries(parent, skipBuiltin)
  local out = {}
  local d = D[parent]
  if not d or not d.items then
    return out
  end
  for _, item in ipairs(d.items) do
    local child = D[item]
    if child and not child.destroyed and not (skipBuiltin and child.builtin) and item.Visible ~= false then
      out[#out + 1] = {
        id = uidOf(item),
        caption = plainCaption(item.Caption),
        shortcut = shortcutText(rawget(child.props, "shortcut")),
        enabled = item.Enabled ~= false,
        checked = item.Checked == true,
        children = menuEntries(item, false),
      }
    end
  end
  return out
end

local function scriptMenu(which)
  if type(which) == "number" then
    local menu = live(which)
    return menu
  end
  if which == "main" then
    return D[mainForm].props.menu
  end
  if not memoryView then
    return nil
  end
  if which == "popup" then
    return findOwned(memoryView, "debuggerpopup", false)
  end
  return D[memoryView].props.menu
end

local function mainMenuEntries(menu)
  local out = {}
  local d = D[menu.Items]
  if not d or not d.items then
    return out
  end
  for _, item in ipairs(d.items) do
    local child = D[item]
    if child and not child.destroyed and item.Visible ~= false then
      if child.builtin then
        local added = menuEntries(item, true)
        if #added > 0 then
          local name = tostring(rawget(child.props, "name") or "")
          out[#out + 1] = {
            id = uidOf(item),
            caption = plainCaption(item.Caption),
            merge = (string.lower(name):gsub("^mi", "")),
            enabled = true,
            checked = false,
            children = added,
          }
        end
      else
        out[#out + 1] = {
          id = uidOf(item),
          caption = plainCaption(item.Caption),
          shortcut = shortcutText(rawget(child.props, "shortcut")),
          enabled = item.Enabled ~= false,
          checked = item.Checked == true,
          children = menuEntries(item, false),
        }
      end
    end
  end
  return out
end

__manifoldMenuSnapshot = function(which)
  local menu = scriptMenu(which)
  if not menu then
    return {}
  end
  if which == "main" then
    return mainMenuEntries(menu)
  end
  return menuEntries(menu.Items, which == "menu")
end

__manifoldMenuPopup = function(which)
  local menu = scriptMenu(which)
  if menu then
    call(D[menu].props.onpopup, menu)
  end
end

__manifoldMenuClick = function(id)
  local item, d = live(id)
  if not item then
    return
  end
  if item.AutoCheck then
    item.Checked = not item.Checked
  end
  call(d.props.onclick, item)
end

local alignNames = { alnone = 0, altop = 1, albottom = 2, alleft = 3, alright = 4, alclient = 5, alcustom = 6 }
local bevelNames = { bvnone = 0, bvlowered = 1, bvraised = 2, bvspace = 3 }
local alignmentNames = { taleftjustify = 0, tarightjustify = 1, tacenter = 2 }
local layoutNames = { tltop = 0, tlcenter = 1, tlbottom = 2 }
local drawnKinds = {
  label = true, button = true, edit = true, memo = true, checkbox = true, togglebox = true, listbox = true,
  panel = true, groupbox = true, splitter = true, combobox = true, treeview = true, cecustombutton = true,
  paintbox = true, image = true, listview = true,
}
local defaultSizes = {
  panel = { 170, 50 }, label = { 65, 17 }, edit = { 80, 23 }, memo = { 150, 90 }, button = { 75, 25 },
  cecustombutton = { 75, 25 }, checkbox = { 90, 19 }, togglebox = { 90, 25 }, listbox = { 100, 100 },
  combobox = { 100, 23 }, groupbox = { 185, 105 }, radiogroup = { 185, 105 }, splitter = { 5, 100 },
  treeview = { 121, 97 }, listview = { 250, 150 }, image = { 90, 90 }, paintbox = { 105, 105 },
  progressbar = { 100, 20 }, trackbar = { 100, 25 }, scrollbox = { 150, 150 }, pagecontrol = { 200, 200 },
  tabsheet = { 200, 200 }, statusbar = { 100, 23 }, synedit = { 150, 150 },
}

local function enum(value, names, fallback)
  if type(value) == "number" then
    return value
  end
  if value == nil then
    return fallback
  end
  return names[lower(tostring(value))] or fallback
end

local function num(value)
  return tonumber(value) or 0
end

local function alignOf(control)
  return enum(D[control].props.align, alignNames, 0)
end

local function shown(control)
  local visible = D[control].props.visible
  return visible == nil or visible == true
end

local function centred(value)
  if type(value) == "number" then
    return value >= 4
  end
  return lower(tostring(value or "")):find("center") ~= nil
end

local function colorValue(value)
  local color = tonumber(value)
  if not color or color < 0 or color > 0xFFFFFF then
    return nil
  end
  return color
end

local function fontOf(control)
  local current = control
  while current and D[current] do
    local cd = D[current]
    local font = cd.props.font
    if font and D[font] and (D[font].changed or cd.props.parentfont == false) then
      return D[font].props
    end
    current = cd.parent
  end
  return {}
end

local function fontInfo(control)
  local props = fontOf(control)
  local size = tonumber(props.size) or 9
  if size <= 0 then
    size = 9
  end
  local name = lower(tostring(props.name or ""))
  return {
    size = size,
    bold = lower(tostring(props.style or "")):find("fsbold") ~= nil,
    mono = name:find("consolas") ~= nil or name:find("courier") ~= nil or name:find("mono") ~= nil or name:find("lucida console") ~= nil,
    color = colorValue(props.color),
  }
end

local function measure(control, text)
  local font = fontInfo(control)
  text = tostring(text or "")
  local native = rawget(_G, "__manifoldTextExtent")
  if native then
    local w, h = native(text, font.size, font.bold, font.mono)
    if w then
      return w, h
    end
  end
  local px = font.size * 96 / 72
  local longest, lines = 0, 0
  for line in (text .. "\n"):gmatch("(.-)\r?\n") do
    lines = lines + 1
    longest = math.max(longest, #line)
  end
  return math.floor(longest * px * (font.mono and 0.55 or 0.52) + 0.5), math.floor(math.max(1, lines) * px * 1.25 + 0.5)
end

local function isPanel(control, d)
  return d.class == "panel" or inherits(control, "panel")
end

local function insets(control, d)
  local n = num(d.props.borderwidth)
  if isPanel(control, d) then
    local width = tonumber(control.BevelWidth) or 1
    if enum(control.BevelOuter, bevelNames, 2) ~= 0 then
      n = n + width
    end
    if enum(control.BevelInner, bevelNames, 0) ~= 0 then
      n = n + width
    end
    return n, n, n, n
  end
  if d.class == "groupbox" or d.class == "radiogroup" then
    local _, h = measure(control, "Xg")
    return n + 2, n + h + 2, n + 2, n + 2
  end
  return n, n, n, n
end

local function clientSize(control, d)
  if inherits(control, "form") then
    return d.hostW or tonumber(d.props.clientwidth) or num(d.props.width), d.hostH or tonumber(d.props.clientheight) or num(d.props.height)
  end
  return num(d.props.width), num(d.props.height)
end

local function spacingOf(d)
  local bs = d.props.borderspacing
  local s = bs and D[bs] and D[bs].props or {}
  local around = num(s.around)
  return num(s.left) + around, num(s.top) + around, num(s.right) + around, num(s.bottom) + around
end

local function constrain(d, w, h)
  local cs = d.props.constraints
  local s = cs and D[cs] and D[cs].props
  if s then
    if num(s.maxwidth) > 0 then
      w = math.min(w, num(s.maxwidth))
    end
    if num(s.maxheight) > 0 then
      h = math.min(h, num(s.maxheight))
    end
    w = math.max(w, num(s.minwidth))
    h = math.max(h, num(s.minheight))
  end
  return math.max(0, math.floor(w + 0.5)), math.max(0, math.floor(h + 0.5))
end

local function autoSizeOf(control, d)
  local kind = d.class
  local auto = d.props.autosize
  if auto == nil then
    auto = kind == "label" or kind == "checkbox" or kind == "edit" or kind == "combobox"
  end
  if not auto then
    return nil, nil
  end
  if kind == "label" then
    local w, h = measure(control, controlText(control, d))
    if d.props.wordwrap then
      return nil, h
    end
    return w, h
  end
  if kind == "checkbox" then
    local w, h = measure(control, controlText(control, d))
    return w + 21, math.max(h, 17)
  end
  if kind == "edit" or kind == "combobox" then
    local _, h = measure(control, "Xg")
    return nil, h + 8
  end
  return nil, nil
end

local function insertBefore(c1, c2, align)
  local p1, p2 = D[c1].props, D[c2].props
  local b1, b2 = D[c1].base or {}, D[c2].base or {}
  if align == 1 then
)MLUA"),
            std::string_view(R"MLUA(    return num(p1.top) < num(p2.top) or (num(p1.top) == num(p2.top) and num(b1.top) < num(b2.top))
  elseif align == 3 then
    return num(p1.left) < num(p2.left) or (num(p1.left) == num(p2.left) and num(b1.left) < num(b2.left))
  elseif align == 2 then
    local e1, e2 = num(p1.top) + num(p1.height), num(p2.top) + num(p2.height)
    return e1 > e2 or (e1 == e2 and num(b1.top) + num(b1.height) > num(b2.top) + num(b2.height))
  elseif align == 4 then
    local e1, e2 = num(p1.left) + num(p1.width), num(p2.left) + num(p2.width)
    return e1 > e2 or (e1 == e2 and num(b1.left) + num(b1.width) > num(b2.left) + num(b2.width))
  end
  return false
end

local function alignList(parent, pd, align, start)
  local list = {}
  if start and D[start] and rawequal(D[start].parent, parent) and alignOf(start) == align and shown(start) then
    list[1] = start
  end
  for _, control in ipairs(pd.alignOrder or pd.controls) do
    local cd = D[control]
    if not rawequal(control, start) and cd and not cd.destroyed and rawequal(cd.parent, parent) and alignOf(control) == align and shown(control) then
      local at = 1
      while at <= #list and not insertBefore(control, list[at], align) do
        at = at + 1
      end
      table.insert(list, at, control)
    end
  end
  return list
end

local realign
local function place(control, d, align, rect)
  local p = d.props
  local sl, st, sr, sb = spacingOf(d)
  local w, h = num(p.width), num(p.height)
  local aw, ah = autoSizeOf(control, d)
  if align == 1 or align == 2 then
    w = rect.r - rect.l - sl - sr
    h = ah or h
  elseif align == 3 or align == 4 then
    h = rect.b - rect.t - st - sb
    w = aw or w
  else
    w = rect.r - rect.l - sl - sr
    h = rect.b - rect.t - st - sb
  end
  w, h = constrain(d, w, h)
  local l, t
  if align == 1 then
    l, t = rect.l + sl, rect.t + st
    rect.t = t + h + sb
  elseif align == 2 then
    l, t = rect.l + sl, rect.b - sb - h
    rect.b = t - st
  elseif align == 3 then
    l, t = rect.l + sl, rect.t + st
    rect.l = l + w + sr
  elseif align == 4 then
    l, t = rect.r - sr - w, rect.t + st
    rect.r = l - sl
  else
    l, t = rect.l + sl, rect.t + st
  end
  local resized = p.width ~= w or p.height ~= h
  p.left, p.top, p.width, p.height = l, t, w, h
  if resized then
    d.paintDirty = true
    d.resizePending = true
    realign(control, nil)
  end
end

local aligning = setmetatable({}, { __mode = "k" })
realign = function(parent, start)
  local pd = parent and D[parent]
  if not pd or pd.destroyed or aligning[parent] or #pd.controls == 0 then
    return
  end
  aligning[parent] = true
  local cw, ch = clientSize(parent, pd)
  local il, it, ir, ib = 0, 0, 0, 0
  if not inherits(parent, "form") then
    il, it, ir, ib = insets(parent, pd)
  end
  local rect = { l = il, t = it, r = math.max(il, cw - ir), b = math.max(it, ch - ib) }
  for align = 1, 5 do
    for _, control in ipairs(alignList(parent, pd, align, start)) do
      place(control, D[control], align, rect)
    end
  end
  for _, control in ipairs(pd.controls) do
    local cd = D[control]
    if cd and not cd.destroyed and shown(control) then
      local align = alignOf(control)
      if align == 0 or align == 6 then
        local aw, ah = autoSizeOf(control, cd)
        local w, h = constrain(cd, aw or num(cd.props.width), ah or num(cd.props.height))
        if w ~= cd.props.width or h ~= cd.props.height then
          cd.props.width, cd.props.height = w, h
          cd.paintDirty = true
          cd.resizePending = true
          realign(control, nil)
        end
      end
    end
  end
  aligning[parent] = nil
end

function layout.created(obj, d)
  if not inherits(obj, "control") or inherits(obj, "form") then
    return
  end
  local size = defaultSizes[d.class] or { 100, 100 }
  local p = d.props
  p.width = p.width or size[1]
  p.height = p.height or size[2]
  p.left = p.left or 0
  p.top = p.top or 0
  d.base = { left = p.left, top = p.top, width = p.width, height = p.height }
  if d.class == "splitter" and p.align == nil then
    p.align = "alLeft"
  end
end

function layout.parentChanged(obj, old, new)
  if old and D[old] then
    local od = D[old]
    if od.alignOrder then
      removeValue(od.alignOrder, obj)
    end
    realign(old, nil)
  end
  if new and D[new] then
    local nd = D[new]
    nd.alignOrder = nd.alignOrder or {}
    removeValue(nd.alignOrder, obj)
    table.insert(nd.alignOrder, 1, obj)
    realign(new, obj)
  end
end

local boundsKeys = { left = true, top = true, width = true, height = true }
local layoutKeys = {
  align = true, visible = true, autosize = true, caption = true, text = true, borderwidth = true, bevelouter = true,
  bevelinner = true, bevelwidth = true, wordwrap = true, clientwidth = true, clientheight = true, parentfont = true,
}
local ownBoundsKeys = {
  left = true, top = true, width = true, height = true, clientwidth = true, clientheight = true, bevelouter = true,
  bevelinner = true, bevelwidth = true, borderwidth = true,
}

function layout.changed(obj, d, k)
  local class = d.class
  if class == "borderspacing" or class == "constraints" then
    local owner = d.owner
    local od = owner and D[owner]
    if od then
      if od.parent then
        realign(od.parent, nil)
      end
      realign(owner, nil)
    end
    return
  end
  if class == "font" then
    d.changed = true
    local owner = d.owner
    local od = owner and D[owner]
    if od then
      if od.parent then
        realign(od.parent, nil)
      end
      realign(owner, nil)
    end
    return
  end
  if not (boundsKeys[k] or layoutKeys[k]) or not inherits(obj, "control") then
    return
  end
  if boundsKeys[k] then
    d.base = d.base or {}
    d.base[k] = num(d.props[k])
  end
  if k == "align" and d.parent then
    local pd = D[d.parent]
    pd.alignOrder = pd.alignOrder or {}
    removeValue(pd.alignOrder, obj)
    table.insert(pd.alignOrder, 1, obj)
  end
  if d.parent then
    realign(d.parent, obj)
  end
  if ownBoundsKeys[k] and inherits(obj, "wincontrol") then
    realign(obj, nil)
  end
end

function layout.boundsChanged(obj, d)
  d.base = d.base or {}
  for key in pairs(boundsKeys) do
    d.base[key] = num(d.props[key])
  end
  touchForms()
  if d.parent then
    realign(d.parent, obj)
  end
  realign(obj, nil)
end

layout.realign = realign

local penStyles = { pssolid = 0, psdash = 1, psdot = 2, psdashdot = 3, psdashdotdot = 4, psinsideframe = 5, pspattern = 6, psclear = 7 }
local brushStyles = { bssolid = 0, bsclear = 1 }

local function canvasPen(canvas, d)
  local pen = d.props.pen or canvas.Pen
  local p = D[pen].props
  return tonumber(p.color) or 0, math.max(1, tonumber(p.width) or 1), enum(p.style, penStyles, 0) == 7
end

local function canvasBrush(canvas, d)
  local brush = d.props.brush or canvas.Brush
  local p = D[brush].props
  return tonumber(p.color) or 0xFFFFFF, enum(p.style, brushStyles, 0) == 1
end

local function canvasFontInfo(canvas, d)
  local font = d.props.font or canvas.Font
  local p = D[font].props
  local size = tonumber(p.size) or 9
  if size <= 0 then
    size = 9
  end
  local name = lower(tostring(p.name or ""))
  local mono = name:find("consolas") ~= nil or name:find("courier") ~= nil or name:find("mono") ~= nil or name:find("lucida console") ~= nil
  return size, tonumber(p.color) or 0x20000000, lower(tostring(p.style or "")):find("fsbold") ~= nil, mono
end

local function canvasArea(d)
  local owner = d.owner and D[d.owner]
  if not owner then
    return 0, 0
  end
  return num(owner.props.width), num(owner.props.height)
end

local function canvasReset(d)
  local reset = rawget(_G, "__manifoldCanvasReset")
  if reset then
    reset(d.serial)
  end
end

local function canvasShape(canvas, op, x1, y1, x2, y2, usePen, useBrush, rx, ry)
  local native = rawget(_G, "__manifoldCanvasShape")
  if not native then
    return
  end
  local d = D[canvas]
  local pen, width, penClear = 0, 1, true
  if usePen then
    pen, width, penClear = canvasPen(canvas, d)
  end
  local brush, brushClear = 0xFFFFFF, true
  if useBrush then
    brush, brushClear = canvasBrush(canvas, d)
  end
  native(d.serial, op, num(x1), num(y1), num(x2), num(y2), pen, width, penClear, brush, brushClear, num(rx), num(ry))
end

local function pointList(points, ...)
  local flat = {}
  if type(points) == "table" then
    for _, point in ipairs(points) do
      if type(point) == "table" then
        flat[#flat + 1] = num(point.x or point.X or point[1])
        flat[#flat + 1] = num(point.y or point.Y or point[2])
      else
        flat[#flat + 1] = num(point)
      end
    end
  else
    for _, value in ipairs({ points, ... }) do
      flat[#flat + 1] = num(value)
    end
  end
  return flat
end

local function canvasPoly(canvas, closed, points, ...)
  local native = rawget(_G, "__manifoldCanvasPoly")
  if not native then
    return
  end
  local d = D[canvas]
  local pen, width, penClear = canvasPen(canvas, d)
  local brush, brushClear = canvasBrush(canvas, d)
  native(d.serial, closed, pointList(points, ...), pen, width, penClear, brush, closed and brushClear or true)
end

Impl.canvas.line = function(self, x1, y1, x2, y2)
  canvasShape(self, 1, x1, y1, x2, y2, true, false)
  local d = D[self]
  d.penX, d.penY = x2, y2
end
Impl.canvas.moveto = function(self, x, y)
  local d = D[self]
  d.penX, d.penY = x, y
end
Impl.canvas.lineto = function(self, x, y)
  local d = D[self]
  canvasShape(self, 1, d.penX or 0, d.penY or 0, x, y, true, false)
  d.penX, d.penY = x, y
end
Impl.canvas.rect = function(self, x1, y1, x2, y2) canvasShape(self, 2, x1, y1, x2, y2, true, true) end
Impl.canvas.rectangle = Impl.canvas.rect
Impl.canvas.framerect = function(self, x1, y1, x2, y2)
  local native = rawget(_G, "__manifoldCanvasShape")
  if native then
    local d = D[self]
    local brush = canvasBrush(self, d)
    native(d.serial, 2, num(x1), num(y1), num(x2), num(y2), brush, 1, false, 0, true, 0, 0)
  end
end
Impl.canvas.fillrect = function(self, x1, y1, x2, y2)
  local d = D[self]
  local w, h = canvasArea(d)
  if w > 0 and num(x1) <= 0 and num(y1) <= 0 and num(x2) >= w and num(y2) >= h then
    canvasReset(d)
  end
  canvasShape(self, 3, x1, y1, x2, y2, false, true)
end
Impl.canvas.clear = function(self)
  local d = D[self]
  canvasReset(d)
  local w, h = canvasArea(d)
  canvasShape(self, 3, 0, 0, w, h, false, true)
end
Impl.canvas.ellipse = function(self, x1, y1, x2, y2) canvasShape(self, 4, x1, y1, x2, y2, true, true) end
Impl.canvas.roundrect = function(self, x1, y1, x2, y2, rx, ry) canvasShape(self, 5, x1, y1, x2, y2, true, true, rx, ry or rx) end
Impl.canvas.setpixel = function(self, x, y, color)
  local native = rawget(_G, "__manifoldCanvasShape")
  if native then
    native(D[self].serial, 6, num(x), num(y), num(x) + 1, num(y) + 1, tonumber(color) or 0, 1, false, 0, true, 0, 0)
  end
end
Impl.canvas.polygon = function(self, points, ...) canvasPoly(self, true, points, ...) end
Impl.canvas.polyline = function(self, points, ...) canvasPoly(self, false, points, ...) end
Impl.canvas.textout = function(self, x, y, text)
  local native = rawget(_G, "__manifoldCanvasText")
  if not native then
    return
  end
  local d = D[self]
  local size, color, bold, mono = canvasFontInfo(self, d)
  local brush, brushClear = canvasBrush(self, d)
  native(d.serial, num(x), num(y), tostring(text or ""), size, color, bold, mono, brush, brushClear)
end
Impl.canvas.textrect = function(self, rect, x, y, text) Impl.canvas.textout(self, x, y, text) end
Impl.canvas.draw = function(self, x, y, graphic)
  local native = rawget(_G, "__manifoldCanvasDraw")
  if not native or not isObject(graphic) then
    return
  end
  local source = graphic.Canvas
  if source and D[source] then
    native(D[self].serial, D[source].serial, num(x), num(y), 0, 0)
  end
end
Impl.canvas.stretchdraw = function(self, rect, graphic)
  local native = rawget(_G, "__manifoldCanvasDraw")
  if not native or not isObject(graphic) or type(rect) ~= "table" then
    return
  end
)MLUA"),
            std::string_view(R"MLUA(  local source = graphic.Canvas
  if source and D[source] then
    local left, top = num(rect.Left or rect[1]), num(rect.Top or rect[2])
    native(D[self].serial, D[source].serial, left, top, num(rect.Right or rect[3]) - left, num(rect.Bottom or rect[4]) - top)
  end
end
Impl.canvas.copyrect = function(self, dest, source, sourceRect)
  if isObject(source) and D[source] and D[source].class == "canvas" then
    local native = rawget(_G, "__manifoldCanvasDraw")
    if native and type(dest) == "table" then
      native(D[self].serial, D[source].serial, num(dest.Left or dest[1]), num(dest.Top or dest[2]), 0, 0)
    end
  end
end

local function markPaint(self)
  local d = D[self]
  d.paintDirty = true
  touchForms()
end
Impl.paintbox = Impl.paintbox or {}
for _, name in ipairs({ "repaint", "invalidate", "refresh", "update" }) do
  Impl.paintbox[name] = markPaint
end

layout.canvasFreed = function(d)
  local free = rawget(_G, "__manifoldCanvasFree")
  if free then
    free(d.serial)
  end
end

local paintErrors = setmetatable({}, { __mode = "k" })
local function paintSurface(control, d)
  local canvas = control.Canvas
  local cd = canvas and D[canvas]
  if not cd then
    return 0
  end
  if d.class == "paintbox" and d.paintDirty ~= false then
    d.paintDirty = false
    canvasReset(cd)
    if type(d.props.onpaint) == "function" then
      local ok, err = pcall(d.props.onpaint, control)
      if not ok and paintErrors[control] ~= tostring(err) then
        paintErrors[control] = tostring(err)
        print("OnPaint failed: " .. tostring(err))
      end
    end
  end
  return cd.serial
end
layout.paintSurface = paintSurface

local function splitterTarget(splitter, sd)
  local parent = sd.parent
  if not parent or not D[parent] then
    return nil
  end
  local align = alignOf(splitter)
  local sp = sd.props
  local best, bestEdge
  for _, control in ipairs(D[parent].controls) do
    local cd = D[control]
    if not rawequal(control, splitter) and cd and not cd.destroyed and shown(control) and alignOf(control) == align then
      local p = cd.props
      local edge
      if align == 3 then
        edge = num(p.left) + num(p.width)
        if edge <= num(sp.left) + 1 and (not bestEdge or edge > bestEdge) then
          best, bestEdge = control, edge
        end
      elseif align == 4 then
        edge = num(p.left)
        if edge >= num(sp.left) + num(sp.width) - 1 and (not bestEdge or edge < bestEdge) then
          best, bestEdge = control, edge
        end
      elseif align == 1 then
        edge = num(p.top) + num(p.height)
        if edge <= num(sp.top) + 1 and (not bestEdge or edge > bestEdge) then
          best, bestEdge = control, edge
        end
      elseif align == 2 then
        edge = num(p.top)
        if edge >= num(sp.top) + num(sp.height) - 1 and (not bestEdge or edge < bestEdge) then
          best, bestEdge = control, edge
        end
      end
    end
  end
  return best
end

local function stringItems(list)
  local out = {}
  local d = list and D[list]
  if d and d.items then
    for i, text in ipairs(d.items) do
      out[i] = tostring(text)
    end
  end
  return out
end

local function nodeEntries(list)
  local out = {}
  for _, node in ipairs(list or {}) do
    local nd = D[node]
    if nd then
      out[#out + 1] = {
        id = uidOf(node),
        text = tostring(nd.props.text or ""),
        expanded = nd.props.expanded == true,
        haschildren = nd.props.haschildren == true,
        children = nodeEntries(nd.children),
      }
    end
  end
  return out
end

local function fires(p, ...)
  for _, name in ipairs({ ... }) do
    if type(p[name]) == "function" then
      return true
    end
  end
  return false
end

local function controlEntry(control, form)
  local d = D[control]
  local p = d.props
  if d.resizePending then
    d.resizePending = nil
    if type(p.onresize) == "function" then
      local ok, err = pcall(p.onresize, control)
      if not ok then
        print("OnResize failed: " .. tostring(err))
      end
    end
  end
  local font = fontInfo(control)
  local popup = p.popupmenu
  local entry = {
    id = uidOf(control),
    kind = d.class,
    x = num(p.left),
    y = num(p.top),
    w = num(p.width),
    h = num(p.height),
    enabled = p.enabled ~= false,
    color = (p.parentcolor ~= true and p.transparent ~= true) and colorValue(p.color) or nil,
    fontsize = font.size,
    fontcolor = font.color,
    bold = font.bold,
    mono = font.mono,
    cursor = tonumber(p.cursor) or 0,
    click = type(p.onclick) == "function",
    dblclick = type(p.ondblclick) == "function",
    hover = type(p.onmouseenter) == "function" or type(p.onmouseleave) == "function",
    popup = (popup and D[popup] and not D[popup].destroyed) and uidOf(popup) or nil,
    mouse = fires(p, "onmousedown", "onmouseup", "onmousemove"),
    wheel = fires(p, "onmousewheelup", "onmousewheeldown", "onmousewheel"),
    children = {},
  }
  if not drawnKinds[d.class] then
    entry.unsupported = ClassNames[d.class] or d.class
    form.unsupported = form.unsupported or entry.unsupported
  end
  local kind = d.class
  if isPanel(control, d) then
    entry.bevel = enum(control.BevelOuter, bevelNames, 2)
    entry.bevelinner = enum(control.BevelInner, bevelNames, 0)
    entry.bevelwidth = tonumber(control.BevelWidth) or 1
    entry.bevelcolor = colorValue(p.bevelcolor)
    entry.text = plainCaption(p.caption or "")
  elseif kind == "label" then
    entry.text = plainCaption(controlText(control, d))
    entry.alignment = enum(p.alignment, alignmentNames, 0)
    entry.layout = enum(p.layout, layoutNames, 0)
    entry.wordwrap = p.wordwrap == true
    entry.color = (p.parentcolor == false and p.transparent == false) and colorValue(p.color) or nil
  elseif kind == "listbox" or kind == "combobox" then
    entry.items = stringItems(control.Items)
    entry.itemindex = tonumber(p.itemindex) or -1
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
  elseif kind == "memo" or kind == "synedit" then
    entry.text = control.Text
    entry.readonly = control.ReadOnly == true
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
  elseif kind == "edit" then
    entry.text = control.Text
    entry.readonly = control.ReadOnly == true
    entry.hint = tostring(p.texthint or "")
    entry.password = p.passwordchar ~= nil and p.passwordchar ~= "" and p.passwordchar ~= "#0"
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
  elseif kind == "splitter" then
    local target = splitterTarget(control, d)
    entry.resizes = target and uidOf(target) or nil
    entry.minsize = tonumber(p.minsize) or 30
    entry.align = alignOf(control)
  elseif kind == "paintbox" or kind == "image" then
    entry.canvas = paintSurface(control, d)
  elseif kind == "treeview" then
    local items = p.items
    entry.nodes = (items and D[items]) and nodeEntries(D[items].roots) or {}
    local selected = p.selected
    entry.selected = (selected and D[selected] and not D[selected].destroyed) and uidOf(selected) or nil
    entry.rightclickselect = p.rightclickselect == true
    entry.autoexpand = p.autoexpand == true
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
  elseif kind == "listview" then
    entry.viewstyle = enum(p.viewstyle, { vsicon = 0, vssmallicon = 1, vslist = 2, vsreport = 3 }, 0)
    entry.itemindex = tonumber(p.itemindex) or -1
    entry.gridlines = p.gridlines == true
    entry.checkboxes = p.checkboxes == true
    entry.headers = p.showcolumnheaders ~= false
    entry.frame = enum(p.borderstyle, { bsnone = 0, bssingle = 1 }, 1) ~= 0
    entry.columns = {}
    for i, column in ipairs(D[control.Columns].items) do
      local cp = D[column].props
      entry.columns[i] = {
        caption = plainCaption(cp.caption or ""),
        width = tonumber(cp.width) or 50,
        alignment = enum(cp.alignment, alignmentNames, 0),
        visible = cp.visible ~= false,
      }
    end
    entry.rows = {}
    for i, item in ipairs(D[control.Items].items) do
      local ip = D[item].props
      entry.rows[i] = { id = uidOf(item), text = tostring(ip.caption or ""), sub = stringItems(ip.subitems), checked = ip.checked == true }
    end
  else
    entry.text = plainCaption(controlText(control, d))
  end
  if kind == "checkbox" or kind == "togglebox" then
    entry.checked = p.checked == true
  end
  for _, child in ipairs(d.controls) do
    local cd = D[child]
    if cd and not cd.destroyed and shown(child) then
      entry.children[#entry.children + 1] = controlEntry(child, form)
    end
  end
  return entry
end

__manifoldFormSnapshot = function()
  local out = {}
  for _, form in ipairs(forms) do
    local d = D[form]
    if d and d.script and not d.destroyed and d.props.visible then
      local p = d.props
      local constraints = p.constraints and D[p.constraints] and D[p.constraints].props or {}
      local menu = p.menu
      local font = fontInfo(form)
      local entry = {
        id = uidOf(form),
        caption = tostring(form.Caption or ""),
        left = num(p.left),
        top = num(p.top),
        width = tonumber(p.width) or 400,
        height = tonumber(p.height) or 300,
        clientwidth = tonumber(p.clientwidth),
        clientheight = tonumber(p.clientheight),
        center = centred(p.position),
        raise = d.raise or 0,
        minwidth = num(constraints.minwidth),
        minheight = num(constraints.minheight),
        menu = (menu and D[menu] and not D[menu].destroyed) and uidOf(menu) or nil,
        color = colorValue(p.color),
        keydown = type(p.onkeydown) == "function",
        fontsize = font.size,
        fontcolor = font.color,
        bold = font.bold,
        mono = font.mono,
        controls = {},
      }
      for _, child in ipairs(d.controls) do
        local cd = D[child]
        if cd and not cd.destroyed and shown(child) then
          entry.controls[#entry.controls + 1] = controlEntry(child, entry)
        end
      end
      out[#out + 1] = entry
    end
  end
  return out
end

__manifoldFormSet = function(id, field, value)
  local control, d = live(id)
  if not control then
    return
  end
  touchForms()
  if field == "itemindex" then
    d.props.itemindex = tonumber(value) or -1
    d.props.text = nil
  elseif field == "checked" then
    d.props.checked = value == "1"
    d.props.state = value == "1" and 1 or 0
  elseif field == "text" then
    if d.class == "memo" then
      local lines = control.Lines
      D[lines].items = splitLines(value)
    else
      d.props.text = tostring(value or "")
    end
  elseif field == "expanded" then
    d.props.expanded = value == "1"
  elseif field == "selected" then
    local node = live(tonumber(value))
    d.props.selected = node
    if node and d.props.autoexpand == true then
      D[node].props.expanded = true
    end
  elseif field == "width" or field == "height" then
    local size = math.max(0, tonumber(value) or 0)
    d.props[field] = size
    d.base = d.base or {}
    d.base[field] = size
    if d.parent then
      realign(d.parent, nil)
    end
    realign(control, nil)
  end
end

__manifoldFormFire = function(id, event, a, b, c)
  local control, d = live(id)
  if not control then
    return
  end
  local p = d.props
  if event == "select" then
    if d.class == "combobox" then
      call(p.onchange, control)
      call(p.onselect, control)
    else
      call(p.onclick, control)
      call(p.onselectionchange, control, true)
    end
  elseif event == "nodechange" then
    call(p.onchange, control, p.selected)
    call(p.onselectionchanged, control)
  elseif event == "itemselect" then
    local old = D[control.Items].items[(tonumber(a) or -1) + 1]
    if old then
      call(p.onselectitem, control, old, false)
    end
    local item = control.Selected
    if item then
      call(p.onselectitem, control, item, true)
      call(p.onchange, control, item, 2)
    end
)MLUA"),
            std::string_view(R"MLUA(  elseif event == "itemcheck" then
    call(p.onitemchecked, control, (live(a)))
  elseif event == "columnclick" then
    call(p.oncolumnclick, control, D[control.Columns].items[(tonumber(a) or -1) + 1])
  elseif event == "dblclick" then
    call(p.ondblclick, control)
  elseif event == "change" then
    call(p.onchange, control)
    if d.class == "checkbox" then
      call(p.onclick, control)
    end
  elseif event == "click" then
    call(p.onclick, control)
  elseif event == "mouseenter" then
    call(p.onmouseenter, control)
  elseif event == "mouseleave" then
    call(p.onmouseleave, control)
  elseif event == "moved" then
    call(p.onmoved, control)
  elseif event == "mousedown" then
    call(p.onmousedown, control, a, b, c)
  elseif event == "mouseup" then
    call(p.onmouseup, control, a, b, c)
  elseif event == "mousemove" then
    call(p.onmousemove, control, a, b)
  elseif event == "wheelup" or event == "wheeldown" then
    local named = event == "wheelup" and p.onmousewheelup or p.onmousewheeldown
    if type(named) == "function" then
      named(control, a, b)
    else
      call(p.onmousewheel, control, event == "wheelup" and 120 or -120, a, b)
    end
  elseif event == "keydown" then
    call(p.onkeydown, control, a)
  end
end

__manifoldFormClose = function(id)
  local form = live(id)
  if form then
    form.close()
  end
end

__manifoldFormBounds = function(id, left, top, width, height, clientWidth, clientHeight)
  local form, d = live(id)
  if not form then
    return
  end
  local p = d.props
  p.left, p.top = left, top
  p.width, p.height = width, height
  if p.clientwidth then
    p.clientwidth = clientWidth or width
  end
  if p.clientheight then
    p.clientheight = clientHeight or height
  end
  d.hostW, d.hostH = clientWidth, clientHeight
  realign(form, nil)
  touchForms()
end

getFormCount = getFormCount or function() return #forms end
getForm = getForm or function(index) return forms[(tonumber(index) or -1) + 1] end

registerFormAddNotification = registerFormAddNotification or function(fn)
  local handle = new("object")
  formNotifications[handle] = fn
  Impl.object.unregister = Impl.object.unregister or function(self) formNotifications[self] = nil end
  return handle
end
unregisterFormAddNotification = unregisterFormAddNotification or function(handle)
  if handle ~= nil then
    formNotifications[handle] = nil
  end
end

setMethodProperty = setMethodProperty or function(object, property, fn) object[property] = fn end
getMethodProperty = getMethodProperty or function(object, property) return object[property] end
setProperty = setProperty or function(object, property, value) object[property] = value end
getProperty = getProperty or function(object, property) return object[property] end

inheritsFromObject = inheritsFromObject or function(v) return isObject(v) or type(v) == "userdata" end
inheritsFromComponent = inheritsFromComponent or function(v) return inherits(v, "component") end
inheritsFromControl = inheritsFromControl or function(v) return inherits(v, "control") end
inheritsFromWinControl = inheritsFromWinControl or function(v) return inherits(v, "wincontrol") end

darkMode = darkMode or function() return true end
translate = translate or function(text) return text end
getTranslationFolder = getTranslationFolder or function() return "" end
loadPOFile = loadPOFile or function() return false end

showSelectionList = showSelectionList or function(title, caption, list, allowCustom)
  if __manifoldHeadless then
    return -1, ""
  end
  local items = {}
  if isObject(list) and D[list] and D[list].items then
    for i, text in ipairs(D[list].items) do
      items[i] = tostring(text)
    end
  elseif type(list) == "table" then
    for i, text in ipairs(list) do
      items[i] = tostring(text)
    end
  end
  return __manifoldShowSelectionList(title, caption, items, allowCustom)
end

__manifoldNewThreadObject = function()
  return new("thread")
end

local addressListObject = new("addresslist")
D[addressListObject].props.name = "Addresslist"
for _, entry in ipairs({ { "List", "treeview" }, { "Header", "headercontrol" } }) do
  local control = construct(entry[2], addressListObject)
  D[control].props.name = entry[1]
  D[addressListObject].props[lower(entry[1])] = control
end
D[mainForm].props.addresslist = addressListObject

__manifoldSetFallback("list", function(list, key)
  if type(key) ~= "string" then
    return nil
  end
  local value = addressListObject[key]
  if type(value) == "function" then
    return function(first, ...)
      if rawequal(first, list) or rawequal(first, addressListObject) then
        return value(addressListObject, ...)
      end
      return value(first, ...)
    end
  end
  return value
end)

local legacyClasses = {}
for name in pairs(classes) do
  legacyClasses[name] = true
end
for _, name in ipairs({ "memoryrecord", "addresslist", "timer", "memscan", "foundlist", "tablefile", "memorystream", "stream", "hotkey", "genericHotkey" }) do
  legacyClasses[lower(name)] = true
end
local globalMeta = getmetatable(_G) or {}
local previousIndex = globalMeta.__index
globalMeta.__index = function(globals, name)
  if type(name) == "string" then
    local class, method = name:match("^(%a+)_(%a[%w]*)$")
    if class and legacyClasses[lower(class)] and method:match("^%l") then
      local fn = function(object, ...)
        local member = object and object[method]
        if type(member) == "function" then
          return member(object, ...)
        end
        return member
      end
      rawset(globals, name, fn)
      return fn
    end
  end
  if type(previousIndex) == "function" then
    return previousIndex(globals, name)
  elseif type(previousIndex) == "table" then
    return previousIndex[name]
  end
  return nil
end
setmetatable(_G, globalMeta)

__manifoldLcl = {
  new = new,
  construct = construct,
  data = D,
  classes = classes,
  impl = Impl,
  isObject = isObject,
  inherits = inherits,
  forms = forms,
}

getSettings = getSettings or function(path)
  local section = (path == nil or path == "") and "Cheat Engine" or tostring(path)
  local value = setmetatable({}, {
    __index = function(_, name)
      return __manifoldSettingRead(section, tostring(name))
    end,
    __newindex = function(_, name, v)
      __manifoldSettingWrite(section, tostring(name), v == nil and "" or tostring(v))
    end,
  })
  local settings = { Path = section, Value = value, ClassName = "TSettings" }
  settings.getValue = function(name) return value[name] end
  settings.setValue = function(name, v) value[name] = v end
  settings.destroy = noop
  return settings
end
)MLUA"),
        };
        constexpr std::array<std::string_view, 1> kScript3 = {
            std::string_view(R"MLUA(-- Cheat Engine's .NET data collector, on top of manifold.dotnet. Cheat Engine
-- names a class by its module handle and TypeDef token; here those are the
-- handles and tokens manifold.dotnet.classes reports. The collector loads the
-- .NET runtime on first use. Methods take the collector as an optional first
-- argument, so both dc.enumDomains() and dc:enumDomains() work.

local collector
local warnedAllObjects = false

local function dotnet()
  local api = manifold.dotnet
  if not api.isLoaded() and not api.load() then
    return nil
  end
  return api
end

local byToken
local generation

-- The classes by "module:token", rebuilt when the class list changes. The
-- process id and the generation (one per reading of the classes) tell a
-- restarted game from the one before, whose module and count are the same.
local function classByToken(api, module, token)
  local status = api.status()
  local key = tostring(status.processId) .. ":" .. tostring(status.generation) .. ":" .. tostring(status.module) .. ":" .. tostring(status.classes)
  if not byToken or generation ~= key then
    byToken = {}
    for _, class in ipairs(api.classes() or {}) do
      if class.token ~= 0 then
        byToken[class.module .. ":" .. class.token] = class
      end
    end
    generation = key
  end
  return byToken[tostring(module) .. ":" .. tostring(token)]
end

local function instanceFields(api, name)
  local fields = {}
  local seen = 0
  while name and name ~= "" and seen < 48 do
    local class = api.class(name)
    if not class then break end
    for _, field in ipairs(class.fields) do
      fields[#fields + 1] = { Offset = field.offset, FieldType = field.typeCode, Name = field.name, FieldTypeClassName = field.type, IsStatic = false }
    end
    name = class.parent
    seen = seen + 1
  end
  table.sort(fields, function(a, b) return a.Offset < b.Offset end)
  return fields
end

local function layout(api, address, start, className)
  local pointer = api.status().pointerSize or 8
  local data = { StartAddress = start, ClassName = className, ObjectType = 0x12, ElementType = 0, ElementSize = 0, CountOffset = 0, FirstElementOffset = 0 }
  local object = api.object(start)
  if object and object.kind == "array" then
    data.ObjectType = 0x1D
    data.ElementType = object.elementType
    data.ElementSize = object.elementSize
    data.CountOffset = pointer
    data.FirstElementOffset = object.data - start
  elseif object and object.kind == "string" then
    data.ObjectType = 0x0E
    data.ElementType = 0x03
    data.ElementSize = 2
    data.CountOffset = pointer
    data.FirstElementOffset = pointer + 4
  end
  data.Fields = instanceFields(api, className)
  return data
end

local function method(name, body)
  return function(first, ...)
    if first == collector then
      return body(...)
    end
    return body(first, ...)
  end
end

collector = {
  enumDomains = method("enumDomains", function()
    if not dotnet() then return {} end
    return { { DomainHandle = 1, Name = "DefaultDomain" } }
  end),

  enumModuleList = method("enumModuleList", function()
    local api = dotnet()
    if not api then return {} end
    local list = {}
    for _, module in ipairs(api.modules() or {}) do
      list[#list + 1] = { ModuleHandle = module.handle, BaseAddress = module.base, Name = module.path ~= "" and module.path or module.name }
    end
    return list
  end),

  enumTypeDefs = method("enumTypeDefs", function(moduleHandle)
    local api = dotnet()
    if not api then return {} end
    local list = {}
    for _, class in ipairs(api.classes() or {}) do
      if class.module == moduleHandle and class.token ~= 0 then
        list[#list + 1] = { TypeDefToken = class.token, Name = class.name, Flags = 0, Extends = 0 }
      end
    end
    return list
  end),

  getTypeDefMethods = method("getTypeDefMethods", function(moduleHandle, token)
    local api = dotnet()
    local class = api and classByToken(api, moduleHandle, token)
    local details = class and api.class(class.name)
    if not details then return {} end
    local list = {}
    for _, m in ipairs(details.methods) do
      list[#list + 1] = { Name = m.name, MethodToken = m.token, Attributes = m.static and 0x10 or 0, ImplementationFlags = 0, ILCode = 0,
        NativeCode = m.address or 0, SecondaryNativeCode = {} }
    end
    return list
  end),

  getMethodParameters = method("getMethodParameters", function(moduleHandle, methodToken)
    local api = dotnet()
    if not api then return {} end
    for _, class in ipairs(api.classes() or {}) do
      if class.module == moduleHandle then
        local details = api.class(class.name)
        for _, m in ipairs(details and details.methods or {}) do
          if m.token == methodToken then
            local list = {}
            for i = 1, m.params do
              list[i] = { Name = "arg" .. i, CType = 0 }
            end
            return list
          end
        end
      end
    end
    return {}
  end),

  getTypeDefParent = method("getTypeDefParent", function(moduleHandle, token)
    local api = dotnet()
    local class = api and classByToken(api, moduleHandle, token)
    local details = class and api.class(class.name)
    local parent = details and details.parent ~= "" and api.class(details.parent)
    if not parent then return nil end
    return { ModuleHandle = parent.module, TypedefToken = parent.token }
  end),

  getTypeDefData = method("getTypeDefData", function(moduleHandle, token)
    local api = dotnet()
    local class = api and classByToken(api, moduleHandle, token)
    if not class then return nil end
    return { ClassName = class.name, ObjectType = 0x12, ElementType = 0, ElementSize = 0, CountOffset = 0, FirstElementOffset = 0,
      Fields = instanceFields(api, class.name) }
  end),

  getAddressData = method("getAddressData", function(address)
    local api = dotnet()
    local found = api and api.classAt(address)
    if not found then return nil end
    return layout(api, address, found.address, found.class)
  end),

  enumAllObjectsOfType = method("enumAllObjectsOfType", function(moduleHandle, token)
    local api = dotnet()
    local class = api and classByToken(api, moduleHandle, token)
    if not class then return {} end
    return api.instances(class.handle) or {}
  end),

  enumAllObjects = method("enumAllObjects", function()
    -- Every object of a game is millions of entries; the counts per type and
    -- the objects of one type are what manifold.dotnet.heap and instances give.
    if not warnedAllObjects then
      warnedAllObjects = true
      manifold.log.warning("enumAllObjects is not built in: use manifold.dotnet.heap() for the counts per type and enumAllObjectsOfType or manifold.dotnet.instances for the objects of one type")
    end
    return nil
  end),
}

rawset(_G, "getDotNetDataCollector", function()
  return collector
end)
)MLUA"),
        };
        constexpr std::array<std::string_view, 1> kScript4 = {
            std::string_view(R"MLUA(if string.split == nil then
  string.split = function(text, separator)
    text = tostring(text)
    if separator == nil or separator == "" then
      return text
    end
    local parts, start = {}, 1
    while true do
      local first, last = string.find(text, separator, start, true)
      if not first then
        parts[#parts + 1] = string.sub(text, start)
        break
      end
      parts[#parts + 1] = string.sub(text, start, first - 1)
      start = last + 1
    end
    return table.unpack(parts)
  end
end

if string.startsWith == nil then
  string.startsWith = function(text, prefix)
    return string.sub(tostring(text), 1, #prefix) == prefix
  end
end

if string.endsWith == nil then
  string.endsWith = function(text, suffix)
    return suffix == "" or string.sub(tostring(text), -#suffix) == suffix
  end
end

-- Pascal's Trim, as Cheat Engine's string.trim: drops every character up to and including space at both ends.
if string.trim == nil then
  string.trim = function(text)
    return (string.match(tostring(text), "^[%z\1- ]*(.-)[%z\1- ]*$"))
  end
end

-- ∑ Cheat Engine 7.x embeds Lua 5.3 without the 5.1 compatibility switches and adds the old names itself at
--   startup (LuaHandler.pas, InitializeLua: "loadstring=load", "unpack=table.unpack", "math.pow=...", and
--   frexp/cosh/sinh/tanh in Pascal). Tables use them, e.g. assert(loadstring(code)); Manifold had none of them
--   (owner report). The same definitions, same behaviour. bit32, table.getn, table.maxn and setfenv/getfenv
--   are absent in Cheat Engine too and stay absent.
if loadstring == nil then
  loadstring = load
end
if unpack == nil then
  unpack = table.unpack
end
if package.loaders == nil then
  package.loaders = package.searchers
end
if string.gfind == nil then
  string.gfind = string.gmatch
end
if math.log10 == nil then
  math.log10 = function(v) return math.log(v, 10) end
end
if math.pow == nil then
  math.pow = function(x, y) return x ^ y end
end
if math.atan2 == nil then
  math.atan2 = math.atan
end
if math.ldexp == nil then
  math.ldexp = function(x, exp) return x * 2.0 ^ exp end
end
if math.mod == nil then
  math.mod = math.fmod
end
if math.cosh == nil then
  math.cosh = function(x)
    local e = math.exp(x)
    return (e + 1 / e) / 2
  end
end
if math.sinh == nil then
  math.sinh = function(x)
    local e = math.exp(x)
    return (e - 1 / e) / 2
  end
end
if math.tanh == nil then
  math.tanh = function(x)
    if x > 20 then return 1.0 end
    if x < -20 then return -1.0 end
    local e = math.exp(2 * x)
    return (e - 1) / (e + 1)
  end
end
if math.frexp == nil then
  -- x = m * 2^e with 0.5 <= |m| < 1, as C's frexp; 0, infinities and NaN come back with exponent 0.
  math.frexp = function(x)
    x = x + 0.0
    if x == 0 or x ~= x or x == math.huge or x == -math.huge then
      return x, 0
    end
    local e = math.floor(math.log(math.abs(x), 2)) + 1
    local half = e // 2
    local m = x * 2.0 ^ -half * 2.0 ^ -(e - half)
    if math.abs(m) >= 1 then
      m, e = m / 2, e + 1
    elseif math.abs(m) < 0.5 then
      m, e = m * 2, e - 1
    end
    return m, math.tointeger(e) or e
  end
end

-- module() and package.seeall, which Cheat Engine's Lua DLL keeps (built with LUA_COMPAT_MODULE) and Lua 5.4
-- removed: module(name, ...) makes or reuses the module table, runs the options on it and makes it the
-- environment (_ENV) of the chunk that called it, as Lua 5.3's ll_module does.
if package.seeall == nil then
  package.seeall = function(module)
    local meta = getmetatable(module)
    if meta == nil then
      meta = {}
      setmetatable(module, meta)
    end
    meta.__index = _G
    return module
  end
end
if module == nil and debug ~= nil and debug.getinfo and debug.setupvalue then
  local debug = debug
  module = function(name, ...)
    local loaded = package.loaded
    local result = loaded[name]
    if type(result) ~= "table" then
      result = _G
      for part in string.gmatch(name, "[^%.]+") do
        local next = rawget(result, part)
        if next == nil then
          next = {}
          rawset(result, part, next)
        elseif type(next) ~= "table" then
          error("name conflict for module '" .. name .. "'", 2)
        end
        result = next
      end
      loaded[name] = result
    end
    if result._NAME == nil then
      result._M = result
      result._NAME = name
      result._PACKAGE = string.match(name, "^(.*%.)") or ""
    end
    local caller = debug.getinfo(2, "f")
    if caller and caller.func then
      for i = 1, math.huge do
        local upvalue = debug.getupvalue(caller.func, i)
        if upvalue == nil then
          break
        end
        if upvalue == "_ENV" then
          debug.setupvalue(caller.func, i, result)
          break
        end
      end
    end
    for _, option in ipairs({ ... }) do
      option(result)
    end
    return result
  end
end
)MLUA"),
        };
        constexpr std::array<std::string_view, 2> kScript5 = {
            std::string_view(R"MLUA(local lcl = __manifoldLcl
local D = lcl.data
local new, construct, classes, Impl = lcl.new, lcl.construct, lcl.classes, lcl.impl

local function define(name, fn)
  if rawget(_G, name) == nil then
    rawset(_G, name, fn)
  end
end

local function fill(list, items)
  list.clear()
  for _, text in ipairs(items) do
    list.add(text)
  end
  return list
end

define("getProcesslist", function(list)
  local processes = getProcessList()
  if list == nil then
    return processes
  end
  local ids = {}
  for pid in pairs(processes) do
    ids[#ids + 1] = pid
  end
  table.sort(ids)
  local items = {}
  for i, pid in ipairs(ids) do
    items[i] = string.format("%08X-%s", pid, processes[pid])
  end
  return fill(list, items)
end)

define("getWindowlist", function(list)
  local windows = __manifoldWindowList()
  if list == nil then
    local result = {}
    for _, window in ipairs(windows) do
      local captions = result[window.pid] or {}
      captions[#captions + 1] = window.caption
      result[window.pid] = captions
    end
    return result
  end
  local items = {}
  for i, window in ipairs(windows) do
    items[i] = string.format("%08X-%s", window.pid, window.caption)
  end
  return fill(list, items)
end)

define("getThreadlist", function(list)
  local threads = __manifoldThreadList()
  if list == nil then
    return threads
  end
  local items = {}
  for i, id in ipairs(threads) do
    items[i] = string.format("%X", id)
  end
  return fill(list, items)
end)

define("readShortInteger", function(address, signed)
  local value = readByte(address)
  if value and signed and value >= 0x80 then
    value = value - 0x100
  end
  return value
end)

define("writeShortInteger", function(address, value)
  return writeByte(address, value)
end)

local refs, nextRef = {}, 0
define("createRef", function(...)
  nextRef = nextRef + 1
  refs[nextRef] = table.pack(...)
  return nextRef
end)
define("getRef", function(id)
  local values = refs[id]
  if values then
    return table.unpack(values, 1, values.n)
  end
end)
define("destroyRef", function(id)
  refs[id] = nil
end)

define("setGlobalVariable", function(name, value)
  rawset(_G, name, value)
end)
define("getGlobalVariable", function(name)
  return rawget(_G, name)
end)

define("getNextAllocNumber", function(script)
  local text = lcl.isObject(script) and script.Text or tostring(script or "")
  local highest
  for number in text:lower():gmatch("alloc%s*%(%s*newmem(%d*)") do
    local value = tonumber(number) or 1
    if highest == nil or value > highest then
      highest = value
    end
  end
  return highest and highest + 1 or 0
end)

define("translateID", function(id, original)
  if original ~= nil then
    return original
  end
  return id
end)

define("loadModule", function(path)
  return injectDLL(path)
end)

define("loadTable", function(source, merge)
  if merge then
    return __manifoldLoadTable(source, true)
  end
  local path, temporary = __manifoldStageTable(source)
  if not path then
    return nil, temporary
  end
  createTimer(1, function()
    __manifoldLoadTable(path, false, temporary)
  end)
  return true
end)

define("createThreadSuspended", function(fn, ...)
  local thread = createThread(function(self, ...)
    while D[self] and D[self].suspended and not self.Terminated do
      sleep(10)
    end
    if not self.Terminated then
      return fn(self, ...)
    end
  end, ...)
  if D[thread] then
    D[thread].suspended = true
  end
  return thread
end)

define("createThreadNewState", function(script)
  local chunk, problem = load(script, "=new state", "t")
  if not chunk then
    error(problem, 2)
  end
  return createThread(function(self)
    return chunk(self)
  end)
end)

local function current()
  return (coroutine.running())
end

local function waitUntil(test, timeout)
  if test() then
    return true
  end
  if not __manifoldCanWait() then
    return false
  end
  timeout = tonumber(timeout)
  local deadline = timeout and timeout >= 0 and timeout < 0xFFFFFFFF and getTickCount() + timeout or nil
  repeat
    sleep(1)
    if test() then
      return true
    end
  until deadline and getTickCount() >= deadline
  return false
end

Impl.criticalsection = {
  enter = function(self)
    local d, me = D[self], current()
    waitUntil(function() return d.holder == nil or d.holder == me end)
    d.holder = me
    d.depth = (d.depth or 0) + 1
  end,
  tryenter = function(self)
    local d, me = D[self], current()
    if d.holder ~= nil and d.holder ~= me then
      return false
    end
    d.holder = me
    d.depth = (d.depth or 0) + 1
    return true
  end,
  leave = function(self)
    local d = D[self]
    if (d.depth or 0) > 0 then
      d.depth = d.depth - 1
      if d.depth == 0 then
        d.holder = nil
      end
    end
  end,
}
define("createCriticalSection", function()
  return new("criticalsection")
end)

Impl.event = {
  setevent = function(self) D[self].signaled = true end,
  resetevent = function(self) D[self].signaled = false end,
  waitfor = function(self, timeout)
    local d = D[self]
    if waitUntil(function() return d.signaled end, timeout) then
      if not d.manualReset then
        d.signaled = false
      end
      return wrSignaled or 0
    end
    return wrTimeout or 1
  end,
}
define("createEvent", function(manualReset, initialState)
  local event = new("event")
  D[event].manualReset = manualReset and true or false
  D[event].signaled = initialState and true or false
  return event
end)

Impl.semaphore = {
  acquire = function(self)
    local d = D[self]
    waitUntil(function() return d.count > 0 end)
    d.count = d.count - 1
  end,
  release = function(self, count)
    local d = D[self]
    d.count = d.count + (tonumber(count) or 1)
  end,
}
define("createSemaphore", function(count)
  local semaphore = new("semaphore")
  D[semaphore].count = tonumber(count) or 1
  return semaphore
end)

Impl.multireadexclusivewritesynchronizer = {
  beginread = function(self)
    local d, me = D[self], current()
    waitUntil(function() return d.writer == nil or d.writer == me end)
    d.readers = (d.readers or 0) + 1
  end,
  endread = function(self)
    local d = D[self]
    d.readers = math.max((d.readers or 0) - 1, 0)
  end,
  beginwrite = function(self)
    local d, me = D[self], current()
    waitUntil(function() return (d.writer == nil or d.writer == me) and (d.readers or 0) == 0 end)
    d.writer = me
    d.writes = (d.writes or 0) + 1
    return true
  end,
  endwrite = function(self)
    local d = D[self]
    d.writes = (d.writes or 1) - 1
    if d.writes <= 0 then
      d.writer, d.writes = nil, 0
    end
  end,
}
define("createMultiReadExclusiveWriteSynchronizer", function()
  return new("multireadexclusivewritesynchronizer")
end)

local function decode(address)
  if type(address) == "string" then
    address = getAddress(address)
  end
  local text = disassemble(address)
  local _, opcode, bytes = splitDisassembledString(text)
  local mnemonic, parameters = tostring(opcode or ""):match("^%s*(%S*)%s*(.-)%s*$")
  local m = (mnemonic or ""):lower()
  local byteTable = {}
  for hex in tostring(bytes or ""):gmatch("%x%x") do
    byteTable[#byteTable + 1] = tonumber(hex, 16)
  end
  local isJump = m:sub(1, 1) == "j" or m:sub(1, 4) == "loop"
  local data = {
    address = address,
    opcode = mnemonic or "",
    parameters = parameters or "",
    description = "",
    commentsoverride = "",
    bytes = byteTable,
    modrmValueType = dvtNone or 0,
    modrmValue = 0,
    parameterValueType = dvtNone or 0,
    parameterValue = 0,
    isJump = isJump,
    isCall = m == "call",
    isRet = m:sub(1, 3) == "ret",
    isRep = m:sub(1, 3) == "rep",
    isConditionalJump = (isJump and m ~= "jmp"),
  }
  if (isJump or data.isCall) and parameters ~= "" then
    local target = getAddressSafe(parameters)
    if target then
      data.parameterValue = target
      data.parameterValueType = dvtAddress or 1
    end
  end
  return text, data
end

Impl.disassembler = {
  disassemble = function(self, address)
    local text, data = decode(address)
    D[self].props.lastdisassembledata = data
    return text
  end,
  getlastdisassembledata = function(self)
    return D[self].props.lastdisassembledata or {}
  end,
  decodelastparameterstostring = function()
    return ""
  end,
}
define("createDisassembler", function()
  return new("disassembler")
end)
local defaultDisassembler
define("getDefaultDisassembler", function()
  defaultDisassembler = defaultDisassembler or new("disassembler")
  return defaultDisassembler
end)
local visibleDisassembler
define("getVisibleDisassembler", function()
  visibleDisassembler = visibleDisassembler or new("disassembler")
  return visibleDisassembler
end)

define("createMemoryView", function()
  return getMemoryViewForm()
end)

define("createAutoAssemblerForm", function(script)
  __manifoldOpenAutoAssembler(script or "")
  return new("tfrmautoinject")
end)

define("createCalendar", function(owner) return construct("calendar", owner) end)
define("createColorBox", function(owner) return construct("combobox", owner) end)
define("createDiagram", function(owner) return construct("diagram", owner) end)
define("createVirtualStringTree", function(owner) return construct("customcontrol", owner) end)
define("createRegion", function() return new("region") end)

local function classKey(name)
  if lcl.isObject(name) then
    return D[name].class
  end
  local key = tostring(name or ""):lower():gsub("^t", "")
  return key
end

local special = {
  form = function() return createForm(false) end,
  ceform = function() return createForm(false) end,
  stringlist = function() return createStringlist() end,
  memorystream = function() return createMemoryStream() end,
  stringstream = function() return createStringStream("") end,
  timer = function(owner) return createTimer(owner, false) end,
}

define("createClass", function(name)
  local key = classKey(name)
  if special[key] then
    return special[key]()
  end
  if classes[key] then
    return new(key)
  end
  return nil
end)

define("createComponentClass", function(name, owner)
  local key = classKey(name)
  if special[key] then
    return special[key](owner)
  end
  if classes[key] then
    return construct(key, owner)
  end
  return nil
end)

define("getPropertyList", function(class)
  local key = classKey(class)
  local names, seen, visited = {}, {}, {}
  while key and classes[key] and not visited[key] do
    visited[key] = true
    for prop in pairs(classes[key].props or {}) do
      if not seen[prop] then
        seen[prop] = true
        names[#names + 1] = prop
      end
    end
    key = classes[key].parent
  end
  table.sort(names)
  return fill(createStringlist(), names)
end)

local autoAttach, autoAttachTimer
local function autoAttachTick()
  local wanted = D[autoAttach].items
  if #wanted == 0 or getOpenedProcessID() ~= 0 then
    return
  end
  local processes = getProcessList()
  for _, name in ipairs(wanted) do
    local lowered = tostring(name):lower()
    for pid, process in pairs(processes) do
      if tostring(process):lower() == lowered then
        openProcess(pid)
        return
      end
    end
  end
end
define("getAutoAttachList", function()
  if not autoAttach then
    autoAttach = createStringlist()
    autoAttachTimer = createTimer(nil, false)
    autoAttachTimer.Interval = 1000
    autoAttachTimer.OnTimer = autoAttachTick
    D[autoAttach].onChange = function(_, d)
      autoAttachTimer.Enabled = #d.items > 0
    end
  end
  return autoAttach
end)

local settingsForm
define("getSettingsForm", function()
  if not settingsForm then
    settingsForm = new("form")
    D[settingsForm].props.name = "frmSettings"
    D[settingsForm].props.caption = "Settings"
  end
  return settingsForm
end)

local timers = {}
local function standInTimer(index)
  local timer = timers[index]
  if not timer then
    timer = new("timer")
    D[timer].props.enabled = true
    timers[index] = timer
  end
)MLUA"),
            std::string_view(R"MLUA(  D[timer].props.interval = select(index, __manifoldTimerIntervals())
  return timer
end
define("getFreezeTimer", function() return standInTimer(1) end)
define("getUpdateTimer", function() return standInTimer(2) end)

local globalMeta = getmetatable(_G) or {}
local previousIndex = globalMeta.__index
local compatStub = __manifoldCompatStub
globalMeta.__index = function(globals, name)
  local value
  if type(previousIndex) == "function" then
    value = previousIndex(globals, name)
  elseif type(previousIndex) == "table" then
    value = previousIndex[name]
  end
  if value == nil and type(name) == "string" then
    value = compatStub(name)
  end
  return value
end
setmetatable(_G, globalMeta)
)MLUA"),
        };
        constexpr std::array<std::string_view, 20> kManifold0 = {
            std::string_view(R"MLUA(local native = ...

local SOURCE = debug.getinfo(1, "S").source
-- ∑ Restarted at 1 for the first release (it had counted to 33 during development).
local API_VERSION = 2
local EMPTY = setmetatable({}, { __newindex = function() error("read-only", 2) end })
local RUNTIME_CODE = "ME-7002"

local LEVELS = { "debug", "info", "success", "warning", "error" }
local RANK = {}
for index, name in ipairs(LEVELS) do
  RANK[name] = index
end

local MEMORY_TYPES = { "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "float", "double", "pointer", "bytes", "raw", "string" }
local SIMPLE_READS = { i8 = true, u8 = true, i16 = true, u16 = true, i32 = true, u32 = true, i64 = true, u64 = true, float = true, double = true, pointer = true, string = true }
local INTEGER_RANGES = {
  i8 = { -0x80, 0x7F },
  u8 = { 0, 0xFF },
  i16 = { -0x8000, 0x7FFF },
  u16 = { 0, 0xFFFF },
  i32 = { -0x80000000, 0x7FFFFFFF },
  u32 = { 0, 0xFFFFFFFF },
}
local MAX_BYTES = 0x1000000
local MAX_STRING = 0x100000

local ErrorMeta = { __name = "manifold.error" }

local function errorText(object)
  local text = tostring(rawget(object, "message")) .. " (" .. tostring(rawget(object, "code")) .. ")"
  local where = rawget(object, "where")
  if type(where) == "string" and where ~= "" then
    text = where .. ": " .. text
  end
  return text
end
ErrorMeta.__tostring = errorText

local function isError(value)
  return type(value) == "table" and getmetatable(value) == ErrorMeta
end

local function callerFrame()
  for level = 2, 64 do
    local info = debug.getinfo(level, "Sln")
    if not info then
      return nil
    end
    if info.source ~= SOURCE and info.what ~= "C" then
      return info
    end
  end
  return nil
end

local function chunkName(info)
  local source = info.source or ""
  local first = source:sub(1, 1)
  if first == "@" then
    return source:match("([^\\/]+)$") or source:sub(2)
  end
  if first == "=" then
    return source:sub(2)
  end
  return info.short_src
end

local function whereOf(info)
  if not info then
    return ""
  end
  if info.currentline and info.currentline > 0 then
    return chunkName(info) .. ":" .. info.currentline
  end
  return chunkName(info)
end

local function makeError(code, message, where, traceback)
  return setmetatable({
    code = code,
    message = message,
    where = where or whereOf(callerFrame()),
    traceback = traceback,
  }, ErrorMeta)
end

local function result(value, code, message)
  if value == nil and code ~= nil then
    return nil, makeError(code, message)
  end
  return value
end

local function misuse(text)
  local code, message = native.fail("LuaApiMisuse", text)
  error(makeError(code, message), 0)
end

local function isInteger(value)
  return math.type(value) == "integer" or (math.type(value) == "float" and math.tointeger(value) ~= nil)
end

local CHECKS = {
  any = function() return true end,
  string = function(value) return type(value) == "string" end,
  number = function(value) return type(value) == "number" end,
  integer = isInteger,
  boolean = function(value) return type(value) == "boolean" end,
  table = function(value) return type(value) == "table" end,
  ["function"] = function(value) return type(value) == "function" end,
  address = function(value) return type(value) == "string" or isInteger(value) end,
  choice = function(value) return isInteger(value) or type(value) == "table" end,
}

local NOUNS = {
  any = "a value",
  string = "a string",
  number = "a number",
  integer = "an integer",
  boolean = "a boolean",
  table = "a table",
  ["function"] = "a function",
  address = "an address (an integer or an address expression)",
  choice = "an item number (a list of them for a combo with multi = true)",
}

local function kindOf(value)
  if value == nil then
    return "nil"
  end
  if isError(value) then
    return "an error object"
  end
  if math.type(value) == "float" then
    return "the number " .. tostring(value)
  end
  return type(value)
end

local function distance(left, right)
  if left == right then
    return 0
  end
  local previous = {}
  for j = 0, #right do
    previous[j] = j
  end
  for i = 1, #left do
    local current = { [0] = i }
    local a = left:byte(i)
    for j = 1, #right do
      local cost = a == right:byte(j) and 0 or 1
      current[j] = math.min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost)
    end
    previous = current
  end
  return previous[#right]
end

local function suggestion(members, key)
  if type(key) ~= "string" then
    return nil
  end
  local wanted = key:lower()
  local best, bestScore
  for name in pairs(members) do
    if type(name) == "string" then
      local candidate = name:lower()
      local score = distance(wanted, candidate)
      if candidate:sub(1, #wanted) == wanted or wanted:sub(1, #candidate) == candidate then
        score = math.min(score, 1)
      end
      if not bestScore or score < bestScore or (score == bestScore and name < best) then
        best, bestScore = name, score
      end
    end
  end
  if best and bestScore <= math.max(2, #key // 3) then
    return best
  end
  return nil
end

local function sortedNames(members)
  local names = {}
  for name in pairs(members) do
    if type(name) == "string" then
      names[#names + 1] = name
    end
  end
  table.sort(names)
  return names
end

local function strict(path)
  local members = setmetatable({}, {
    __index = function(owned, key)
      local text = path .. "." .. tostring(key) .. " does not exist"
      local near = suggestion(owned, key)
      if near then
        text = text .. " (did you mean " .. path .. "." .. near .. "?)"
      else
        text = text .. " (" .. path .. " has " .. table.concat(sortedNames(owned), ", ") .. ")"
      end
      misuse(text)
    end,
  })
  local proxy = setmetatable({}, {
    __index = members,
    __newindex = function(_, key)
      misuse(path .. " is read-only, so " .. path .. "." .. tostring(key) .. " cannot be set")
    end,
    __pairs = function()
      return next, members, nil
    end,
    __tostring = function()
      return path
    end,
    __metatable = false,
  })
  return proxy, members
end

local function prepare(params)
  local prepared = {}
  for index, param in ipairs(params) do
    local types = {}
    for part in param.type:gmatch("[^|]+") do
      types[#types + 1] = part
    end
    local nouns = {}
    for _, kind in ipairs(types) do
      nouns[#nouns + 1] = NOUNS[kind]
    end
    local entry = {
      name = param.name,
      types = types,
      noun = table.concat(nouns, " or "),
      optional = param.optional,
      oneOf = param.oneOf,
      keys = param.keys,
    }
    if param.oneOf then
      entry.oneOfSet = {}
      for _, value in ipairs(param.oneOf) do
        entry.oneOfSet[value] = true
      end
    end
    if param.keys then
      entry.keyList = table.concat(sortedNames(param.keys), ", ")
    end
    prepared[index] = entry
  end
  return prepared
end

local function paramText(params, varargs)
  local names = {}
  for _, param in ipairs(params) do
    names[#names + 1] = param.name .. (param.optional and "?" or "")
  end
  if varargs then
    names[#names + 1] = "..."
  end
  return table.concat(names, ", ")
end

local function checkValue(path, index, param, value)
  local matched = false
  for _, kind in ipairs(param.types) do
    if CHECKS[kind](value) then
      matched = true
      break
    end
  end
  if not matched then
    misuse(("%s: argument %d (%s) must be %s, got %s"):format(path, index, param.name, param.noun, kindOf(value)))
  end
  if param.oneOfSet and not param.oneOfSet[value] then
    local quoted = {}
    for _, choice in ipairs(param.oneOf) do
      quoted[#quoted + 1] = '"' .. choice .. '"'
    end
    misuse(("%s: argument %d (%s) must be one of %s, got %q"):format(path, index, param.name, table.concat(quoted, ", "), tostring(value)))
  end
  if param.keys then
    for key, option in pairs(value) do
      local kind = type(key) == "string" and param.keys[key]
      if not kind then
        misuse(("%s: %s.%s is not an option (known options: %s)"):format(path, param.name, tostring(key), param.keyList))
      end
      if not CHECKS[kind](option) then
        misuse(("%s: %s.%s must be %s, got %s"):format(path, param.name, key, NOUNS[kind], kindOf(option)))
      end
    end
  end
end

local function guard(path, owner, params, varargs, body)
  local count = #params
  return function(...)
    local given = select("#", ...)
    if given > 0 and rawequal((...), owner) then
      misuse(path .. " is called with a dot, not a colon: " .. path .. "(" .. paramText(params, varargs) .. ")")
    end
    if not varargs and given > count then
      misuse(("%s takes %d argument%s, got %d"):format(path, count, count == 1 and "" or "s", given))
    end
    for index = 1, count do
      local param = params[index]
      local value = (select(index, ...))
      if value == nil then
        if not param.optional then
          misuse(("%s: argument %d (%s) is missing"):format(path, index, param.name))
        end
      else
        checkValue(path, index, param, value)
      end
    end
    return body(...)
  end
end

local entries = {}
local areas = {}
local manifold, rootMembers = strict("manifold")

native.docDefine(nil, "manifold", { kind = "table" })

local function area(name, summary)
  local proxy, members = strict("manifold." .. name)
  local section = { path = "manifold." .. name, name = name, proxy = proxy, members = members }
  areas[#areas + 1] = section
  rawset(rootMembers, name, proxy)
  native.docDefine(nil, section.path, { kind = "table", doc = summary })
  return section
end

local function bind(owner, path, members, name, spec, body, docPath)
  local params = prepare(spec.params or {})
  local wrapper = guard(path, owner, params, spec.varargs, body)
  rawset(members, name, wrapper)
  if docPath then
    native.docBind(wrapper, docPath)
  else
    native.docDefine(wrapper, path, spec)
  end
  return wrapper, params
end

local function define(section, name, spec, body)
  local path = section.path .. "." .. name
  bind(section.proxy, path, section.members, name, spec, body)
  entries[path:sub(#"manifold." + 1)] = true
end

local function constant(name, value, doc, shown)
  rawset(rootMembers, name, value)
  native.docDefine(nil, "manifold." .. name, { kind = "constant", value = shown or tostring(value), doc = doc })
  entries[name] = true
end

local function quote(text)
  return '"' .. (text:gsub('[\\"\n\r\t\0]', { ["\\"] = "\\\\", ['"'] = '\\"', ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t", ["\0"] = "\\0" })) .. '"'
end

local function keyText(key, render)
  if type(key) == "string" and key:match("^[%a_][%w_]*$") then
    return key
  end
  return "[" .. render(key) .. "]"
end

local function keyOrder(a, b)
  local ta, tb = type(a), type(b)
  if ta ~= tb then
    return ta < tb
  end
  if ta == "number" or ta == "string" then
    return a < b
  end
  return tostring(a) < tostring(b)
end

local MAX_ITEMS = 50

local function inspect(value, options)
  local maxDepth = options and options.depth or 3
  local hex = options and options.hex or false
  local seen = {}
  local function render(item, depth)
    local kind = type(item)
    if kind == "string" then
      return quote(item)
    end
    if kind == "number" then
      if hex and math.type(item) == "integer" then
        return ("0x%X"):format(item)
      end
      return tostring(item)
    end
    if kind == "nil" or kind == "boolean" then
      return tostring(item)
    end
    if kind == "function" then
      return "<function>"
    end
    if kind ~= "table" then
      return tostring(item)
    end
    local meta = getmetatable(item)
    if isError(item) or meta == false or (type(meta) == "table" and meta.__tostring) then
      return tostring(item)
    end
    if seen[item] then
      return "<cycle>"
    end
    if depth >= maxDepth then
      return "{...}"
    end
    seen[item] = true
    local parts = {}
)MLUA"),
            std::string_view(R"MLUA(    local length = rawlen(item)
    local skipped = 0
    for index = 1, length do
      if #parts >= MAX_ITEMS then
        skipped = skipped + 1
      else
        parts[#parts + 1] = render(rawget(item, index), depth + 1)
      end
    end
    local keys = {}
    for key in next, item do
      if not (math.type(key) == "integer" and key >= 1 and key <= length) then
        keys[#keys + 1] = key
      end
    end
    table.sort(keys, keyOrder)
    for _, key in ipairs(keys) do
      if #parts >= MAX_ITEMS then
        skipped = skipped + 1
      else
        parts[#parts + 1] = keyText(key, function(k) return render(k, depth + 1) end) .. " = " .. render(rawget(item, key), depth + 1)
      end
    end
    if skipped > 0 then
      parts[#parts + 1] = "... +" .. skipped
    end
    seen[item] = nil
    return "{" .. table.concat(parts, ", ") .. "}"
  end
  return render(value, 0)
end

local currentLevel = "info"

local function emit(level, channel, message, fields)
  if RANK[level] < RANK[currentLevel] then
    return
  end
  local text, traceback, code, where
  if isError(message) then
    text = tostring(rawget(message, "message"))
    traceback = rawget(message, "traceback")
    code = rawget(message, "code")
    where = rawget(message, "where")
  elseif type(message) == "string" then
    text = message
  else
    text = inspect(message)
  end
  local info = callerFrame()
  native.logRecord(RANK[level] - 1, text, channel or "", fields ~= nil and inspect(fields) or "", type(fields) == "table" and fields or nil,
    type(traceback) == "string" and traceback or "", code ~= nil and tostring(code) or "", type(where) == "string" and where or "",
    info and chunkName(info) or "", info and info.name or "", info and info.currentline or 0)
end

local LEVEL_DOCS = {
  debug = "Writes a debug line: details that only matter while working on a script. Hidden unless the level is `debug`.",
  info = "Writes an info line.",
  success = "Writes a success line, in the accent colour.",
  warning = "Writes a warning line, in the warning colour. It also goes to the program log.",
  error = "Writes an error line, in the error colour. It also goes to the program log. An error object as `message` adds its traceback.",
}

local MESSAGE_PARAMS = {
  { name = "message", type = "any" },
  { name = "fields", type = "table", optional = true },
}

local channels = {}

local function channelObject(name)
  local existing = channels[name]
  if existing then
    return existing
  end
  local prefix = ("channel(%q)"):format(name)
  local proxy, members = strict(prefix)
  for _, level in ipairs(LEVELS) do
    bind(proxy, prefix .. "." .. level, members, level, {
      params = MESSAGE_PARAMS,
    }, function(message, fields)
      emit(level, name, message, fields)
    end, "manifold.log.Channel." .. level)
  end
  bind(proxy, prefix .. ".channel", members, "channel", {
    params = { { name = "name", type = "string" } },
  }, function(sub)
    return channelObject(name .. "/" .. sub)
  end, "manifold.log.Channel.channel")
  rawset(members, "name", name)
  channels[name] = proxy
  return proxy
end

local log = area("log", "Coloured, levelled lines in the Lua engine output. Each line shows the time, the level and the channel; `fields` are appended as `{key = value}`. Warnings and errors also go to the program log (Log window and the log file in `logs\\`), with the script line that wrote them.")

for _, level in ipairs(LEVELS) do
  define(log, level, {
    params = MESSAGE_PARAMS,
    returns = "nothing",
    doc = LEVEL_DOCS[level],
  }, function(message, fields)
    emit(level, nil, message, fields)
  end)
end

define(log, "channel", {
  params = { { name = "name", type = "string" } },
  returns = "channel",
  doc = "A channel: an object with the same five level functions that tags every line with `[name]`. The same name gives the same object.",
}, function(name)
  if name == "" then
    misuse("manifold.log.channel: argument 1 (name) must not be empty")
  end
  return channelObject(name)
end)

define(log, "setLevel", {
  params = { { name = "level", type = "string", oneOf = LEVELS } },
  returns = "nothing",
  doc = "Drops lines below `level` for every script until the program closes. The default is `\"info\"`.",
}, function(level)
  currentLevel = level
end)

define(log, "getLevel", {
  returns = "level name",
  doc = "The current level.",
}, function()
  return currentLevel
end)

define(log, "inspect", {
  params = {
    { name = "value", type = "any", optional = true },
    { name = "options", type = "table", optional = true, keys = { depth = "integer", hex = "boolean" } },
  },
  returns = "string",
  doc = "One line of text for any value, the way log lines show `fields`: sorted keys, cycles marked, 50 items per table. `options.depth` (default 3) limits nesting; `options.hex` writes integers in hex.",
}, function(value, options)
  if options and options.depth and options.depth < 1 then
    misuse("manifold.log.inspect: options.depth must be 1 or more")
  end
  return inspect(value, options)
end)

native.docDefine(nil, "manifold.log.Channel", {
  kind = "class",
  title = "Channel objects",
  doc = "`manifold.log.channel(name)` returns an object that follows the same rules: dot calls only, read-only, unknown names raise.",
})
for _, level in ipairs(LEVELS) do
  native.docDefine(nil, "manifold.log.Channel." .. level, {
    params = MESSAGE_PARAMS,
    returns = "nothing",
    doc = "Like `manifold.log." .. level .. "`, tagged with `[name]`.",
  })
end
native.docDefine(nil, "manifold.log.Channel.channel", {
  params = { { name = "name", type = "string" } },
  returns = "channel",
  doc = "A sub-channel named `parent/name`.",
})
native.docDefine(nil, "manifold.log.Channel.name", { kind = "field", type = "string", doc = "The channel's name." })

local output = area("output", "The Lua engine window and its output. With *Show on print* on (the default), every printed or logged line opens the window when it is closed. Set in the window's View menu, it is saved with the settings; a script can change it for the running session only.")

define(output, "show", {
  returns = "nothing",
  doc = "Opens the Lua engine window.",
}, function()
  native.showOutput()
end)

define(output, "clear", {
  returns = "nothing",
  doc = "Clears the output.",
}, function()
  native.clearOutput()
end)

define(output, "getShowOnPrint", {
  returns = "boolean",
  doc = "Whether printing opens the Lua engine window.",
}, function()
  return native.showOnPrint()
end)

define(output, "setShowOnPrint", {
  params = { { name = "enabled", type = "boolean" } },
  returns = "nothing",
  doc = "Turns *Show on print* on or off until the program closes. The saved setting stays as the user set it.",
}, function(enabled)
  native.setShowOnPrint(enabled)
end)

local process = area("process", "The opened process. Without one, every function that needs it returns `nil` and an `ME-1005` error.")

define(process, "isOpen", {
  returns = "boolean",
  doc = "Whether a process is open and still running.",
}, function()
  return native.processIsOpen()
end)

define(process, "info", {
  returns = "`{ id, name, path, is64Bit }`, or nil, error",
  doc = "The opened process.",
}, function()
  return result(native.processInfo())
end)

define(process, "open", {
  params = { { name = "target", type = "string|integer" } },
  returns = "`{ id, name, path, is64Bit }`, or nil, error",
  doc = "Opens a process by id or by executable name (`\"game.exe\"`, or the start of the name).",
}, function(target)
  return result(native.processOpen(target))
end)

define(process, "name", {
  returns = "string, or nil, error",
  doc = "The file name of the process's executable module, such as `\"game.exe\"`: the same name the Cheat Engine global `process` holds, read when it is asked for. Addresses name that module's base as `mainmodule` or, as in Cheat Engine, `$process`, so `\"mainmodule+1A2B30\"` works in memory records, pointers, Auto Assembler scripts and here.",
}, function()
  return result(native.processName())
end)

define(process, "find", {
  params = { { name = "name", type = "string", optional = true } },
  returns = "list of `{ id, name, path, windowTitle, is64Bit }`",
  doc = "Running processes whose executable name contains `name` (any case); all of them without `name`.",
}, function(name)
  return native.processFind(name)
end)

define(process, "mainModule", {
  returns = "`{ name, base, size, path }`, or nil, error",
  doc = "The process's executable module. Its base is what `mainmodule` and `$process` stand for in addresses.",
}, function()
  return result(native.mainModule())
end)

define(process, "module", {
  params = { { name = "name", type = "string" } },
  returns = "`{ name, base, size, path }`, or nil, error",
  doc = "A loaded module by file name, any case. `ME-1016` when it is not loaded.",
}, function(name)
  return result(native.module(name))
end)

define(process, "moduleAt", {
  params = { { name = "address", type = "address" } },
  returns = "`{ name, base, size, path }`, or nil, error",
  doc = "The module that contains the address. `ME-1017` for addresses outside every module: the heap, stacks and code the game allocated itself.",
}, function(address)
  return result(native.moduleAt(address))
end)

define(process, "modules", {
  returns = "list of `{ name, base, size, path }`, or nil, error",
  doc = "All loaded modules.",
}, function()
  return result(native.modules())
end)

define(process, "address", {
  params = { { name = "expression", type = "address" } },
  returns = "integer, or nil, error",
  doc = "Resolves an address expression such as `\"game.exe+10\"` or `\"[[base]+8]+4\"`. The error says which part failed.",
}, function(expression)
  return result(native.address(expression))
end)

define(process, "describe", {
  params = { { name = "address", type = "address" } },
  returns = "string, or nil, error",
  doc = "The address as `module.export+offset`, `module+offset` or hex.",
}, function(address)
  return result(native.describe(address))
end)

local memory = area("memory", "Typed reads and writes in the opened process. Types: `i8` `u8` `i16` `u16` `i32` `u32` `i64` `u64` (integers; `u64` above 2^63 comes back negative, as Lua integers are signed), `float`, `double`, `pointer` (8 or 4 bytes by the process), `bytes` (a list of byte values), `raw` (the bytes as one Lua string, made for `string.unpack`) and `string` (UTF-8 in Lua, UTF-16 in memory with `wide`). A write checks that the value fits the type before it touches memory. Reads of the plain types and of `raw` take a fast path, so they suit loops over many objects.")

local function readOptions(kind, options)
  options = options or EMPTY
  if kind == "bytes" or kind == "raw" then
    if options.count == nil then
      misuse(("manifold.memory.read: type %q needs options.count"):format(kind))
    end
    if options.count < 1 or options.count > MAX_BYTES then
      misuse("manifold.memory.read: options.count must be 1 to " .. MAX_BYTES)
    end
  elseif options.count ~= nil then
    misuse("manifold.memory.read: options.count is for types \"bytes\" and \"raw\"")
  end
  if kind == "string" then
    if options.length ~= nil and (options.length < 1 or options.length > MAX_STRING) then
      misuse("manifold.memory.read: options.length must be 1 to " .. MAX_STRING)
    end
  elseif options.length ~= nil or options.wide ~= nil then
    misuse("manifold.memory.read: options.length and options.wide are for type \"string\"")
  end
  return options
end

define(memory, "read", {
  params = {
    { name = "address", type = "address" },
    { name = "type", type = "string", oneOf = MEMORY_TYPES },
    { name = "options", type = "table", optional = true, keys = { count = "integer", length = "integer", wide = "boolean" } },
  },
  returns = "value, or nil, error",
)MLUA"),
            std::string_view(R"MLUA(  doc = "Reads one value. `bytes` and `raw` need `options.count`; `string` reads up to `options.length` characters (default 256) and stops at the first zero.",
}, function(address, kind, options)
  options = readOptions(kind, options)
  return result(native.read(address, kind, options.count or 0, options.length or 256, options.wide or false))
end)

do
  local slow = rawget(memory.members, "read")
  local nativeRead = native.read
  local function fast(address, kind, options, extra)
    if extra == nil and math.type(address) == "integer" then
      if options == nil and SIMPLE_READS[kind] then
        local value, code, message = nativeRead(address, kind, 0, 256, false)
        if value == nil and code ~= nil then
          return nil, makeError(code, message)
        end
        return value
      end
      if kind == "raw" and type(options) == "table" and next(options, next(options)) == nil then
        local count = rawget(options, "count")
        if math.type(count) == "integer" and count >= 1 and count <= MAX_BYTES then
          local value, code, message = nativeRead(address, "raw", count, 0, false)
          if value == nil and code ~= nil then
            return nil, makeError(code, message)
          end
          return value
        end
      end
    end
    if extra == nil then
      return slow(address, kind, options)
    end
    return slow(address, kind, options, extra)
  end
  rawset(memory.members, "read", fast)
  native.docBind(fast, "manifold.memory.read")
end

local function checkWrite(kind, value, options)
  options = options or EMPTY
  local range = INTEGER_RANGES[kind]
  if range then
    if not isInteger(value) then
      misuse(("manifold.memory.write: argument 3 (value) must be an integer for %s, got %s"):format(kind, kindOf(value)))
    end
    if value < range[1] or value > range[2] then
      misuse(("manifold.memory.write: %s does not fit %s (%d to %d)"):format(tostring(value), kind, range[1], range[2]))
    end
  elseif kind == "i64" or kind == "u64" or kind == "pointer" then
    if not isInteger(value) then
      misuse(("manifold.memory.write: argument 3 (value) must be an integer for %s, got %s"):format(kind, kindOf(value)))
    end
    if kind == "pointer" and native.is64Bit() == false and (value < 0 or value > 0xFFFFFFFF) then
      misuse("manifold.memory.write: " .. tostring(value) .. " does not fit a pointer of a 32-bit process")
    end
  elseif kind == "float" or kind == "double" then
    if type(value) ~= "number" then
      misuse(("manifold.memory.write: argument 3 (value) must be a number for %s, got %s"):format(kind, kindOf(value)))
    end
  elseif kind == "bytes" then
    if type(value) == "table" then
      local count = rawlen(value)
      if count > MAX_BYTES then
        misuse("manifold.memory.write: more than " .. MAX_BYTES .. " bytes")
      end
      for index = 1, count do
        local byte = rawget(value, index)
        if not isInteger(byte) or byte < 0 or byte > 255 then
          misuse(("manifold.memory.write: byte %d is %s, not 0 to 255"):format(index, kindOf(byte) == "number" and tostring(byte) or kindOf(byte)))
        end
      end
    elseif type(value) ~= "string" then
      misuse("manifold.memory.write: argument 3 (value) must be a list of byte values or a string for bytes, got " .. kindOf(value))
    end
  elseif kind == "raw" then
    if type(value) ~= "string" then
      misuse("manifold.memory.write: argument 3 (value) must be a string for raw, got " .. kindOf(value))
    end
  elseif kind == "string" then
    if type(value) ~= "string" then
      misuse("manifold.memory.write: argument 3 (value) must be a string for string, got " .. kindOf(value))
    end
  end
  if kind ~= "string" and (options.wide ~= nil or options.terminate ~= nil) then
    misuse("manifold.memory.write: options.wide and options.terminate are for type \"string\"")
  end
  return options
end

define(memory, "write", {
  params = {
    { name = "address", type = "address" },
    { name = "type", type = "string", oneOf = MEMORY_TYPES },
    { name = "value", type = "any" },
    { name = "options", type = "table", optional = true, keys = { wide = "boolean", terminate = "boolean" } },
  },
  returns = "true, or nil, error",
  doc = "Writes one value. A `string` ends with a zero unless `options.terminate` is false.",
}, function(address, kind, value, options)
  options = checkWrite(kind, value, options)
  return result(native.writeMemory(address, kind, value, options.wide or false, options.terminate ~= false))
end)

define(memory, "scan", {
  params = {
    { name = "pattern", type = "string" },
    { name = "options", type = "table", optional = true, keys = { module = "string", executable = "boolean", writable = "boolean", alignment = "integer", limit = "integer" } },
  },
  returns = "list of integers, or nil, error",
  doc = "Finds a byte pattern such as `\"48 8B 05 ?? ?? ?? ??\"` (`??` is any byte) in the whole process, or only in `options.module`. `options.executable` and `options.writable` set to true keep only such memory, set to false only memory without it, and left out take both. `options.alignment` keeps addresses divisible by it; `options.limit` (1 to 1000000, default 100000) stops after that many hits. The list is in address order, so with a limit it holds the lowest hits. No hit is an empty list, not an error.",
}, function(pattern, options)
  options = options or EMPTY
  if options.alignment ~= nil and options.alignment < 1 then
    misuse("manifold.memory.scan: options.alignment must be 1 or more")
  end
  if options.limit ~= nil and (options.limit < 1 or options.limit > 1000000) then
    misuse("manifold.memory.scan: options.limit must be 1 to 1000000")
  end
  return result(native.scan(pattern, options.module, options.executable, options.writable, options.alignment or 0, options.limit or 100000))
end)

define(memory, "resolveStatic", {
  params = {
    { name = "address", type = "address" },
    { name = "options", type = "table", optional = true, keys = { kind = "string", encoding = "string", pointer = "boolean", offset = "integer", length = "integer" } },
  },
  returns = "`{ address, pointer, instruction, length, text, kind, encoding, indexed, fieldOffset, fieldSize, fieldValue, operandSize, notes }`, or nil, error",
  doc = "The fixed address the instruction at `address` names, decoded for the opened process (32-bit code in a 32-bit game): a memory operand (`mov rax,[rip+x]`, `lea rcx,[rip+x]`, `mov eax,[x]`, `call [rip+x]`), the target of a relative `call`, `jmp` or `jcc`, or a number that is an address (`mov rax,x`, `push x`). Without options a memory operand comes first, then a branch target, and a number only when it points into a module. `options.kind` is `\"auto\"`, `\"memory\"`, `\"branch\"` or `\"immediate\"`; `encoding` `\"relative\"` or `\"absolute\"` insists on one; `offset` and `length` (byte offset of the number in the instruction, instruction length) pick and check a field like the older table-file command. `address` is the static, `pointer` the pointer stored there (nil when unreadable; with `options.pointer` that is `ME-2025`). Errors: `ME-2019` nothing decodes, `ME-2020` no fixed address (no operand, `[rax+8]`, `fs:`/`gs:`), `ME-2021` several, `ME-2022` options that do not fit, `ME-2023` the address is not mapped, `ME-2024` the place is hooked (a `jmp` out of every module). Auto Assembler scripts have the same as `resolvestatic(name, address)`.",
}, function(address, options)
  options = options or EMPTY
  for _, key in ipairs({ "kind", "encoding" }) do
    local allowed = key == "kind" and { auto = true, memory = true, branch = true, immediate = true } or { auto = true, relative = true, absolute = true }
    if options[key] ~= nil and not allowed[options[key]] then
      misuse("manifold.memory.resolveStatic: options." .. key .. " \"" .. tostring(options[key]) .. "\" is not one of the allowed words")
    end
  end
  if options.offset ~= nil and (options.offset < 0 or options.offset > 14) then
    misuse("manifold.memory.resolveStatic: options.offset must be 0 to 14")
  end
  if options.length ~= nil and (options.length < 1 or options.length > 15) then
    misuse("manifold.memory.resolveStatic: options.length must be 1 to 15")
  end
  return result(native.resolveStatic(address, options))
end)

define(memory, "isReadable", {
  params = { { name = "address", type = "address" } },
  returns = "boolean",
  doc = "Whether the page at the address can be read. False without a process or for an address that does not resolve.",
}, function(address)
  return native.isReadable(address)
end)

-- ∑ manifold.record lives in its own function: Manifold.lua's main chunk is
-- close to Lua's limit of 200 locals, and these locals end with it.
do
  local function recordArea()
    local record = area("record", "The memory records of the open cheat table: the address list. A record is found by its id or its description and comes back as a record object; scripts and groups are switched on and off, value records also read and write their value as the address list does (its type, hex display and drop-down list apply). A record object stays bound to its record: when the record is deleted or the table is replaced (new, opened, restored), its functions return `nil` and an `ME-6009` error.")
    local RECORD_DOC = "manifold.record.Record."
    local cache = setmetatable({}, { __mode = "v" })
    local identities = setmetatable({}, { __mode = "k" })
    local KINDS = { value = true, script = true, group = true }

    native.docDefine(nil, "manifold.record.Record", {
      kind = "class",
      title = "Record objects",
      doc = "`find`, `all`, `selected` and `create` return record objects: dot calls only, read-only, unknown names raise. Two objects of the same record are the same object.",
    })
    local METHODS = {
      info = { returns = "table", doc = "What the record is: `{ id, description, kind, active, children, hotkeys, parent, comment }`, `kind` being `value`, `script` or `group`; a value record adds `type` (`byte`, `2bytes`, `4bytes`, `8bytes`, `float`, `double`, `string`, `bytes`, `binary`, `pointer` or its custom type's name), `address` (the expression as written), `offsets` (how many pointer offsets), `resolved` (the address it points to now, when it resolves) and `hex`." },
      getDescription = { returns = "string", doc = "The record's description, as the address list shows it." },
      setDescription = { params = { { name = "description", type = "string" } }, returns = "boolean", doc = "Changes the record's description." },
      getValue = { returns = "value", doc = "Reads the record's value: a number for the number types (an integer for the integer types, also when the address list shows it as hex or as a drop-down entry), otherwise the text as the address list shows it (a string, bytes). The text as the address list shows it comes second, and third the description of the drop-down entry the value matches, when the record has a drop-down list. `nil` and an error when the address does not resolve or cannot be read; `ME-6010` for scripts and groups." },
      setValue = { params = { { name = "value", type = "number|string" } }, returns = "boolean", doc = "Writes the record's value, as typing it into the address list does: a number, or a text in the record's own form (hex digits for a hex record, a drop-down entry's value or description, a string). A frozen record keeps the new value. `nil` and an error when it cannot be written." },
      isActive = { returns = "boolean", doc = "Whether the record is switched on: an enabled script, a frozen value, an active group." },
)MLUA"),
            std::string_view(R"MLUA(      setActive = { params = { { name = "active", type = "boolean" } }, returns = "boolean", doc = "Switches the record on or off, as its box in the address list does: a script runs its `[ENABLE]` or `[DISABLE]` part, a value freezes or thaws, a group follows its options for its children. `true` once the record is in that state. Inside a script's coroutine with asynchronous activation on, it waits for the activation without blocking the program. Asked from inside the record's own switch (from its script or one of its handlers), the new switch runs after the current one and counts as done. `nil` and an error when the record did not end in that state: the script's error, the reason when `onMemRecPreExecute` or the record's `OnActivate` or `OnDeactivate` handler refuses the switch, `ME-6009` when Lua of the switch deleted the record." },
      toggle = { returns = "boolean", doc = "Switches the record to the other state and returns the new one; `nil` and an error when that fails, as for `setActive`." },
      getAddress = { returns = "integer", doc = "The address the record's value is at now (pointers followed). `nil` and an error when it does not resolve; `ME-6010` for scripts and groups." },
      children = { returns = "table", doc = "The records directly below this one, in the address list's order." },
      parent = { returns = "record", doc = "The record this one is below, or nil at the top level." },
      find = { params = { { name = "record", type = "string|table" } }, returns = "record", doc = "A record below this one: by its description at any depth (`script.find(\"Multiplier:\")`), or by a path that starts at this record's children (`script.find({ \"Type:\", \"Multiplier:\" })`). Tables repeat names below every script, so this finds a script's own child however often the name occurs elsewhere. Exactly one record must match: `nil` and `ME-6011` with the count when several do, `ME-6008` when none does." },
      path = { returns = "table", doc = "The descriptions from the top level down to this record, its own last: the path `manifold.record.find` takes. It stays the same when the record's id changes, as long as the groups above it keep their names." },
      remove = { returns = "boolean", doc = "Deletes the record and the records below it from the table. Every record of it that is on is switched off first, the deepest first, so scripts take out their hooks and allocations; each removed record's `OnDestroy` handler runs. `nil` and an error, the record staying in the table, while a record of it is being switched, when one of its scripts fails to switch off or stays on, or when Lua of a switch turned one of its scripts on again; `ME-6009` when that Lua deleted it or replaced the table." },
    }
    for _, name in ipairs({ "info", "getDescription", "setDescription", "getValue", "setValue", "isActive", "setActive", "toggle", "getAddress", "children", "parent", "find", "path", "remove" }) do
      native.docDefine(nil, RECORD_DOC .. name, METHODS[name])
    end
    native.docDefine(nil, RECORD_DOC .. "id", { kind = "field", type = "integer", doc = "The record's id, the number Cheat Engine tables store with it." })

    local recordObject
    local function failure(value, code, message)
      if value == nil then
        return result(nil, code, message)
      end
      return value
    end

    recordObject = function(id)
      local identity = native.recordIdentity()
      local key = identity .. ":" .. id
      if cache[key] then
        return cache[key]
      end
      local prefix = ("record(%d)"):format(id)
      local proxy, members = strict(prefix)
      local function method(name, body)
        bind(proxy, prefix .. "." .. name, members, name, METHODS[name], body, RECORD_DOC .. name)
      end
      method("info", function()
        return failure(native.recordInfo(id, identity))
      end)
      method("getDescription", function()
        local info, code, message = native.recordInfo(id, identity)
        if not info then
          return result(nil, code, message)
        end
        return info.description
      end)
      method("setDescription", function(description)
        return failure(native.recordSetDescription(id, identity, description))
      end)
      method("getValue", function()
        local value, text, entry = native.recordGet(id, identity)
        if value == nil then
          return result(nil, text, entry) -- on failure: nil, code, message
        end
        return value, text, entry
      end)
      method("setValue", function(value)
        return failure(native.recordSet(id, identity, value))
      end)
      method("isActive", function()
        return failure(native.recordIsActive(id, identity))
      end)
      method("setActive", function(active)
        return failure(native.recordSetActive(id, identity, active))
      end)
      method("toggle", function()
        local active, code, message = native.recordIsActive(id, identity)
        if active == nil then
          return result(nil, code, message)
        end
        local done, failed, why = native.recordSetActive(id, identity, not active)
        if not done then
          return result(nil, failed, why)
        end
        return (native.recordIsActive(id, identity))
      end)
      method("getAddress", function()
        return failure(native.recordAddress(id, identity))
      end)
      method("children", function()
        local ids, code, message = native.recordChildren(id, identity)
        if not ids then
          return result(nil, code, message)
        end
        local list = {}
        for index, child in ipairs(ids) do
          list[index] = recordObject(child)
        end
        return list
      end)
      method("parent", function()
        local info, code, message = native.recordInfo(id, identity)
        if not info then
          return result(nil, code, message)
        end
        return info.parent and recordObject(info.parent) or nil
      end)
      method("find", function(key)
        local found, code, message
        if type(key) == "table" then
          local ok, value, failedCode, failedMessage = pcall(native.recordFindPath, key, id, identity)
          if not ok then
            misuse(prefix .. ".find: " .. tostring(value))
          end
          found, code, message = value, failedCode, failedMessage
        else
          found, code, message = native.recordFindBelow(id, identity, key)
        end
        if not found then
          return result(nil, code, message)
        end
        return recordObject(found)
      end)
      method("path", function()
        return failure(native.recordPath(id, identity))
      end)
      method("remove", function()
        return failure(native.recordRemove(id, identity))
      end)
      rawset(members, "id", id)
      cache[key] = proxy
      identities[proxy] = identity
      return proxy
    end

    local function objects(ids)
      local list = {}
      for index, id in ipairs(ids) do
        list[index] = recordObject(id)
      end
      return list
    end

    define(record, "find", {
      params = { { name = "record", type = "integer|string|table" } },
      returns = "record",
      doc = "The record with this id, the one record with this description (as written in the address list), or the one at this path: the descriptions from the top level down, `{ \"Player\", \"Stats\", \"Health\" }`, as `record.path()` gives them. Ids change when records are copied between tables and names repeat in many tables, but a path stays as long as the groups keep their names. A description or a path has to lead to exactly one record: `nil` and `ME-6011` with the count when several match (`findAll` lists the records with a description). `nil` and an `ME-6008` error when there is none.",
    }, function(key)
      local id, code, message
      if type(key) == "table" then
        local ok, value, failedCode, failedMessage = pcall(native.recordFindPath, key)
        if not ok then
          misuse("manifold.record.find: " .. tostring(value))
        end
        id, code, message = value, failedCode, failedMessage
      else
        id, code, message = native.recordFind(key)
      end
      if not id then
        return result(nil, code, message)
      end
      return recordObject(id)
    end)

    define(record, "findAll", {
      params = { { name = "description", type = "string" } },
      returns = "table",
      doc = "Every record with this description, in the address list's order; an empty list when there is none.",
    }, function(description)
      return objects(native.recordFindAll(description))
    end)

    define(record, "all", {
      returns = "table",
      doc = "Every record of the table, groups and their children included, in the address list's order from top to bottom.",
    }, function()
      return objects(native.recordAll())
    end)

    define(record, "selected", {
      returns = "table",
      doc = "The records selected in the address list, in the order they were selected.",
    }, function()
      return objects(native.recordSelected())
    end)

    define(record, "create", {
      params = { { name = "options", type = "table", keys = { kind = "string", description = "string", address = "string", type = "string", script = "string", parent = "any" } } },
      returns = "record",
      doc = "Adds a record at the end of the table, or below `options.parent` (a record object or id). `kind` is `value` (the default), `script` or `group`; a value record takes `address` (an address expression, like in the address list) and `type` (as `info` names them; `4bytes` by default), a script record takes `script` (Auto Assembler text with `[ENABLE]` and `[DISABLE]`).",
    }, function(options)
      local kind = options.kind or "value"
      if not KINDS[kind] then
        misuse(('manifold.record.create: options.kind must be "value", "script" or "group", got %q'):format(tostring(kind)))
      end
      if options.type ~= nil and kind ~= "value" then
        misuse("manifold.record.create: options.type is for value records")
      end
      if options.script ~= nil and kind ~= "script" then
        misuse("manifold.record.create: options.script is for script records")
      end
      local parentId, parentIdentity
      if options.parent ~= nil then
        if math.type(options.parent) == "integer" then
          parentId, parentIdentity = options.parent, native.recordIdentity()
        elseif identities[options.parent] then
          parentId, parentIdentity = options.parent.id, identities[options.parent]
        else
          misuse("manifold.record.create: options.parent must be a record object or a record id, got " .. kindOf(options.parent))
        end
      end
      local ok, id, code, message = pcall(native.recordCreate, kind, options.description or "", kind == "script" and options.script or options.address, options.type, parentId, parentIdentity)
      if not ok then
        misuse("manifold.record.create: " .. tostring(id))
      end
      if not id then
        return result(nil, code, message)
      end
      return recordObject(id)
    end)
  end
  recordArea()
end

local errors = area("error","Error objects and protected calls. An error object is `{ code, message, where, traceback }`; `tostring` gives `where: message (code)`.")

local function fromRaised(problem, traceback)
  if isError(problem) then
    if rawget(problem, "traceback") == nil then
      problem.traceback = traceback
    end
    return problem
  end
  local text = type(problem) == "string" and problem or inspect(problem)
  local where, message = text:match("^([^\n]-:%d+): (.*)$")
  if where then
    where = where:match("([^\\/]+)$") or where
  end
  return makeError(RUNTIME_CODE, message or text, where or "", traceback)
end

local function tracebackOf(level)
  local kept = {}
  for line in debug.traceback("", level):gmatch("[^\n]+") do
    local frame = line:gsub("^%s+", "")
)MLUA"),
            std::string_view(R"MLUA(    if not (frame:find("^manifold:%d+:") or frame == "[C]: in function 'xpcall'" or frame == "(...tail calls...)") then
      kept[#kept + 1] = line
    end
  end
  return table.concat(kept, "\n")
end

define(errors, "create", {
  params = {
    { name = "message", type = "string" },
    { name = "options", type = "table", optional = true, keys = { code = "string" } },
  },
  returns = "error object",
  doc = "A new error object for the calling line. `options.code` sets the script's own code; the default is `ME-7002`.",
}, function(message, options)
  return makeError(options and options.code or RUNTIME_CODE, message)
end)

define(errors, "is", {
  params = { { name = "value", type = "any", optional = true } },
  returns = "boolean",
  doc = "Whether `value` is an error object.",
}, function(value)
  return isError(value)
end)

define(errors, "raise", {
  params = {
    { name = "problem", type = "string|table" },
    { name = "options", type = "table", optional = true, keys = { code = "string" } },
  },
  returns = "never returns",
  doc = "Raises an error object: `problem` itself when it is one, else a new one from the text.",
}, function(problem, options)
  if isError(problem) then
    error(problem, 0)
  end
  if type(problem) ~= "string" then
    misuse("manifold.error.raise: argument 1 (problem) must be a string or an error object, got table")
  end
  error(makeError(options and options.code or RUNTIME_CODE, problem), 0)
end)

define(errors, "try", {
  params = { { name = "fn", type = "function" } },
  varargs = true,
  returns = "true, results..., or false, error",
  doc = "Calls `fn(...)`. On an error it returns false and an error object with the line that raised it and the traceback, instead of stopping the script.",
}, function(fn, ...)
  return xpcall(fn, function(problem)
    return fromRaised(problem, tracebackOf(3))
  end, ...)
end)

define(errors, "check", {
  params = {
    { name = "value", type = "any", optional = true },
    { name = "problem", type = "any", optional = true },
  },
  varargs = true,
  returns = "its arguments",
  doc = "Passes its arguments through when `value` is neither nil nor false, else raises `problem`. Made for `nil, error` results: `local health = manifold.error.check(manifold.memory.read(address, \"i32\"))`.",
}, function(value, problem, ...)
  if value then
    return value, problem, ...
  end
  if isError(problem) then
    error(problem, 0)
  end
  if type(problem) == "string" then
    error(makeError(RUNTIME_CODE, problem), 0)
  end
  error(makeError(RUNTIME_CODE, "manifold.error.check got " .. tostring(value)), 0)
end)

local function raisedHandler(problem)
  return fromRaised(problem, tracebackOf(3))
end

local function protect(kind, fn)
  local reported = false
  local function finish(ok, first, ...)
    if ok then
      return first, ...
    end
    if not reported then
      reported = true
      emit("error", "manifold." .. kind, first)
    end
    return nil
  end
  return function(...)
    return finish(xpcall(fn, raisedHandler, ...))
  end
end

local moduleOwned = {}

local function callerModule()
  local info = callerFrame()
  return info and native.moduleOfSource(info.source) or nil
end

local function own(owner, remove)
  local list = moduleOwned[owner]
  if not list then
    list = {}
    moduleOwned[owner] = list
  end
  local token = {}
  list[token] = remove
  return token
end

local function disown(owner, token)
  local list = moduleOwned[owner]
  if list then
    list[token] = nil
  end
end

local function keyedRegistry(add, remove)
  local byKey = {}
  local keyOf = {}
  local ownerOf = {}
  local function drop(id)
    local removed = remove(id) and true or false
    local key = keyOf[id]
    if key ~= nil then
      byKey[key] = nil
      keyOf[id] = nil
    end
    local owned = ownerOf[id]
    if owned then
      ownerOf[id] = nil
      disown(owned[1], owned[2])
    end
    return removed
  end
  local function register(fn, options)
    local key = options and options.key
    if key ~= nil and byKey[key] ~= nil then
      drop(byKey[key])
    end
    local id = add(fn)
    if key ~= nil then
      byKey[key] = id
      keyOf[id] = key
    end
    local owner = callerModule()
    if owner then
      ownerOf[id] = { owner, own(owner, function() drop(id) end) }
    end
    return id
  end
  return register, drop
end

local KEY_OPTION = { name = "options", type = "table", optional = true, keys = { key = "string" } }
local KEY_TEXT = " `options.key` replaces an earlier registration with the same key, so a module that runs again does not register twice."

local ceSymbolLookup = rawget(_G, "registerSymbolLookupCallback")
local ceSymbolUnlookup = rawget(_G, "unregisterSymbolLookupCallback")
local ceNameLookup = rawget(_G, "registerStructureNameLookup")
local ceNameUnlookup = rawget(_G, "unregisterStructureNameLookup")
local ceDissector = rawget(_G, "registerStructureDissectOverride")
local ceUndissector = rawget(_G, "unregisterStructureDissectOverride")

local symbol = area("symbol", "Symbols that address expressions, the disassembler and Auto Assembler scripts know. A registered symbol belongs to the opened process: closing it or opening another one removes it, so a module registers its symbols again from a `processOpened` handler or when it resolves.")

define(symbol, "register", {
  params = {
    { name = "name", type = "string" },
    { name = "address", type = "address" },
  },
  returns = "true, or nil, error",
  doc = "Registers `name` for `address`, replacing an earlier symbol of that name.",
}, function(name, address)
  if name == "" then
    misuse("manifold.symbol.register: argument 1 (name) must not be empty")
  end
  return result(native.registerSymbol(name, address))
end)

define(symbol, "unregister", {
  params = { { name = "name", type = "string" } },
  returns = "true, or nil, error",
  doc = "Removes a registered symbol. `ME-8002` when there is none of that name.",
}, function(name)
  return result(native.unregisterSymbol(name))
end)

local addSymbolResolver, removeSymbolResolver = keyedRegistry(function(resolver)
  local guarded = protect("symbol", resolver)
  return ceSymbolLookup(function(name)
    local value = guarded(name)
    if math.type(value) == "integer" then
      return value
    end
    return nil
  end, 5)
end, ceSymbolUnlookup)

define(symbol, "addResolver", {
  params = { { name = "resolver", type = "function" }, KEY_OPTION },
  returns = "id",
  doc = "Calls `resolver(name)` for a name that is neither a module, an export nor a registered symbol; it returns an address or nil. Resolvers are asked in the order they were added, and the first address wins. An error in a resolver is logged once." .. KEY_TEXT,
}, function(resolver, options)
  return addSymbolResolver(resolver, options)
end)

define(symbol, "removeResolver", {
  params = { { name = "id", type = "integer" } },
  returns = "boolean",
  doc = "Removes a resolver; false when the id is unknown.",
}, function(id)
  return removeSymbolResolver(id)
end)

local EVENTS = { "processOpened", "processClosed", "unrealReady", "themeChanged" }
local subscribers = { processOpened = {}, processClosed = {}, unrealReady = {}, themeChanged = {} }
local subscriberNames = {}
local nextSubscriber = 1

local subscribe, unsubscribe = keyedRegistry(function(entry)
  local id = nextSubscriber
  nextSubscriber = nextSubscriber + 1
  entry.id = id
  local list = subscribers[entry.name]
  list[#list + 1] = entry
  subscriberNames[id] = entry.name
  return id
end, function(id)
  local name = subscriberNames[id]
  if not name then
    return false
  end
  subscriberNames[id] = nil
  local list = subscribers[name]
  for index, entry in ipairs(list) do
    if entry.id == id then
      table.remove(list, index)
      return true
    end
  end
  return false
end)

local event = area("event", "Notifications from the program. Handlers run in the order they subscribed; an error in one is logged with its traceback and does not stop the others. Handlers run while the program waits, so they should only reset state: long work belongs in a thread or happens on first use.")

define(event, "subscribe", {
  params = {
    { name = "name", type = "string", oneOf = EVENTS },
    { name = "handler", type = "function" },
    KEY_OPTION,
  },
  returns = "id",
  doc = "Calls `handler` on an event. `processOpened` passes the process as `manifold.process.info()` gives it, also when the same process is opened again; `processClosed` passes nothing and comes before the next `processOpened`; `unrealReady` passes nothing and follows when the Unreal tool has resolved the game; `themeChanged` passes the look as `manifold.ui.theme.current()` gives it, after the user, Windows (System mode) or a script changed it, at most once a frame." .. KEY_TEXT,
}, function(name, handler, options)
  return subscribe({ name = name, handler = handler }, options)
end)

define(event, "unsubscribe", {
  params = { { name = "id", type = "integer" } },
  returns = "boolean",
  doc = "Removes a handler; false when the id is unknown.",
}, function(id)
  return unsubscribe(id)
end)

debug.getregistry()["Manifold.Dispatch"] = function(name)
  local list = subscribers[name]
  if not list or #list == 0 then
    return
  end
  local argument = nil
  if name == "processOpened" then
    argument = native.processInfo()
  elseif name == "themeChanged" then
    argument = native.uiThemeCurrent()
  end
  local snapshot = table.move(list, 1, #list, 1, {})
  for _, entry in ipairs(snapshot) do
    local ok, problem = xpcall(entry.handler, raisedHandler, argument)
    if not ok then
      emit("error", "manifold.event", problem)
    end
  end
end

local structure = area("structure", "Hooks into Structure Dissect. Hooks are asked in the order they were added, and the first answer wins. An error in a hook is logged once and counts as no answer.")

local addNameResolver, removeNameResolver = keyedRegistry(function(resolver)
  return ceNameLookup(protect("structure", resolver))
end, ceNameUnlookup)

local addDissector, removeDissector = keyedRegistry(function(dissector)
  local guarded = protect("structure", dissector)
  return ceDissector(function(target, address)
    return guarded(target, address) == true
  end)
end, ceUndissector)

define(structure, "addNameResolver", {
  params = { { name = "resolver", type = "function" }, KEY_OPTION },
  returns = "id",
  doc = "Calls `resolver(address)` when Structure Dissect needs a name for a new structure or for a pointer it expands. It returns a name and, optionally, the address where the object starts, or nil." .. KEY_TEXT,
}, function(resolver, options)
  return addNameResolver(resolver, options)
end)

define(structure, "removeNameResolver", {
  params = { { name = "id", type = "integer" } },
  returns = "boolean",
  doc = "Removes a name resolver; false when the id is unknown.",
}, function(id)
  return removeNameResolver(id)
end)

define(structure, "addDissector", {
  params = { { name = "dissector", type = "function" }, KEY_OPTION },
  returns = "id",
  doc = "Calls `dissector(structure, address)` before Structure Dissect guesses fields. It fills the structure (the Cheat Engine structure object: `addElement`, `autoGuess`, …) and returns true, or returns false to leave it to the next dissector and the guess." .. KEY_TEXT,
}, function(dissector, options)
  return addDissector(dissector, options)
end)

define(structure, "removeDissector", {
  params = { { name = "id", type = "integer" } },
  returns = "boolean",
  doc = "Removes a dissector; false when the id is unknown.",
}, function(id)
  return removeDissector(id)
end)

define(structure, "resolveName", {
  params = { { name = "address", type = "address" } },
  returns = "name and start, nil when no resolver answers, or nil, error",
)MLUA"),
            std::string_view(R"MLUA(  doc = "Asks the name resolvers in order, as Structure Dissect does, and returns the first answer: the name and the address where the object starts (the given address when the resolver names none). Resolvers that Cheat Engine scripts registered are asked too; the program's own Mono lookup is not.",
}, function(address)
  local at, problem = result(native.address(address))
  if at == nil then
    return nil, problem
  end
  for _, hook in ipairs(native.structureHooks(0)) do
    local ok, name, start = pcall(hook, at)
    if ok and type(name) == "string" and name ~= "" then
      return name, math.type(start) == "integer" and start or at
    end
  end
  return nil
end)

define(structure, "dissect", {
  params = {
    { name = "structure", type = "any" },
    { name = "address", type = "address" },
  },
  returns = "boolean, or nil, error",
  doc = "Asks the dissectors in order, as Structure Dissect does before it guesses, to fill `structure` (a Cheat Engine structure object, e.g. from `createStructure`) for the object at `address`. True when one of them filled it.",
}, function(target, address)
  local at, problem = result(native.address(address))
  if at == nil then
    return nil, problem
  end
  for _, hook in ipairs(native.structureHooks(1)) do
    local ok, handled = pcall(hook, target, at)
    if ok and handled == true then
      return true
    end
  end
  return false
end)

local MISUSE_CODE = native.fail("LuaApiMisuse")

local function docResult(value, code, message)
  if value == nil and code == MISUSE_CODE then
    misuse(message)
  end
  return result(value, code, message)
end

local DOC_SPEC = {
  path = "string",
  kind = "string",
  summary = "string",
  doc = "string",
  title = "string",
  type = "string",
  value = "any",
  params = "table",
  varargs = "boolean",
  returns = "string",
  returnsDoc = "string",
  overloads = "table",
  deprecated = "any",
  hidden = "boolean",
  see = "table",
}

local unreal = area("unreal", "Unreal Engine 4 and 5 reflection for 64-bit and 32-bit games, built into the program: it finds the game's name table, object array and reflection layout itself, without an SDK or game offsets, and only reads. Structure Dissect then knows Unreal objects (real property names, nested structs, bitfield bools, the native gaps), the custom types `UE FName`, `UE FString` and `UE Object Index` show names, strings and weak references, and `<Type>_<Property>` symbols such as `Actor_RootComponent` resolve to offsets in every session. It starts by itself when an Unreal game is opened, unless `configure{ autoStart = false }` says otherwise, and the window is in Extensions > Unreal Engine. The queries need it resolved and return `nil` and an `ME-4201` error before that. Types are given by name (`\"Actor\"`, without the U, A or F prefix) or by address.")

local function many(first, ...)
  if first == nil and select("#", ...) > 0 and (...) ~= nil then
    return result(first, ...)
  end
  return first, ...
end

local PATTERN_TEXT = "A pattern is an AOB string, or `{ pattern, offset, length, relative, add, module }`: the hit plus `offset` holds a 32-bit displacement relative to the end of the instruction (`length` bytes long, `offset + 4` by default), or with `relative = false` an absolute pointer of the game's pointer size, which is what 32-bit code holds; `add` is added to the result, and `module` (the executable by default) is where the pattern is searched."

define(unreal, "status", {
  returns = "`{ state, text, failure, objects, types, namePool, objectArray, objectArraySource, legacyNames, doubleVectors, ue3, seconds }`",
  doc = "Where the Unreal tool stands. `state` is `idle`, `notUnreal` (the opened game is no Unreal game), `resolving`, `ready`, `failed` (with `failure`) or `stopped`; `text` is the line the menu shows. The counts and addresses are there once it is ready; `ue3` is true for an Unreal Engine 3 game.",
}, function()
  return native.unrealStatus()
end)

define(unreal, "start", {
  params = { { name = "options", type = "table", optional = true, keys = { wait = "boolean" } } },
  returns = "true, or nil, error",
  doc = "Resolves the opened process again, in any process, not only one that looks like Unreal. It runs in the background and the `unrealReady` event follows; with `options.wait` it resolves before it returns and gives the error when it fails. After `stop` this is what turns the tool on again.",
}, function(options)
  return result(native.unrealStart((options or EMPTY).wait == true))
end)

define(unreal, "stop", {
  returns = "true",
  doc = "Shuts the Unreal tool down: it drops what was resolved, its symbols and dissects stop, and nothing starts by itself again until `start`, also in the next game.",
}, function()
  return native.unrealStop()
end)

define(unreal, "configure", {
  params = {
    { name = "options", type = "table", keys = {
      autoStart = "boolean", dissect = "boolean", names = "boolean", symbols = "boolean", types = "boolean",
      namePool = "any", objectArray = "any", legacyNames = "boolean", layout = "any", patterns = "any",
    } },
  },
  returns = "true, or nil, error",
  doc = "Changes how the tool works; keys that are left out keep their value, and the settings last until the program closes, so an autorun script sets them once. `autoStart` (true): resolve when an Unreal game is opened. `dissect`, `names`, `symbols`, `types` (all true): Structure Dissect fields, structure names, the `<Type>_<Property>` and `GObjects` symbols, the custom types. For a game the search does not find: `namePool` (the FNamePool, or with `legacyNames = true` the TNameEntryArray of UE 4.22 and older) and `objectArray` (FUObjectArray or its ObjObjects) as addresses or expressions, `layout` as a table of offsets that replace what the probes find (`index`, `class`, `name`, `outer`, `super`, `childProperties`, `structSize`, `children`, `childNext`, `functionFlags`, `fField`, `fieldClass`, `fieldName`, `next`, `offset`, `size`, `extra`, `itemSize`, `itemOffset`), and `patterns = { names = {...}, objects = {...} }` searched before the built-in search. " .. PATTERN_TEXT .. " `false` clears an address, the layout or the patterns. A change to the addresses, layout or patterns applies at the next `start`.",
}, function(options)
  return result(native.unrealConfigure(options))
end)

define(unreal, "config", {
  returns = "`{ autoStart, dissect, names, symbols, types, namePool, objectArray, legacyNames, layout, patterns }`",
  doc = "The settings `configure` changes, as it would take them.",
}, function()
  return native.unrealConfig()
end)

define(unreal, "layout", {
  returns = "table of offsets, or nil, error",
  doc = "The layout the tool found: the object array items (`itemSize`, `itemOffset`, `chunked`), UObject (`index`, `class`, `name`, `outer`), UStruct (`super`, `childProperties`, `structSize`, `structSize16` when only its low 16 bits hold the size, `children`, `childNext`, `functionFlags`) and the properties (`fField`, `fieldClass`, `fieldName`, `next`, `offset`, `size`, `extra`, `component` 4 or 8 for float or double vectors) and `ue3` (Unreal Engine 3 rules: the Core package, a 32-bit bool mask, enums as name lists). The keys are the ones `configure{ layout = ... }` takes.",
}, function()
  return result(native.unrealLayout())
end)

define(unreal, "count", {
  returns = "integer, or nil, error",
  doc = "How many slots the object array has in use.",
}, function()
  return result(native.unrealCount())
end)

define(unreal, "objectAt", {
  params = { { name = "index", type = "integer" } },
  returns = "address or nil, or nil, error",
  doc = "The object in a slot of the object array; nil for an empty slot.",
}, function(index)
  return many(native.unrealObjectAt(index))
end)

define(unreal, "isObject", {
  params = { { name = "address", type = "address" } },
  returns = "boolean",
  doc = "Whether a live UObject starts at the address: its index points back at it in the object array. False before the tool is ready.",
}, function(address)
  return native.unrealIsObject(address) == true
end)

define(unreal, "name", {
  params = { { name = "address", type = "address" } },
  returns = "string or nil, or nil, error",
  doc = "The FName stored at an address, with its number as `Name_1`.",
}, function(address)
  return many(native.unrealName(address))
end)

define(unreal, "nameOf", {
  params = { { name = "object", type = "address" } },
  returns = "string or nil, or nil, error",
  doc = "An object's name.",
}, function(object)
  return many(native.unrealNameOf(object))
end)

define(unreal, "fullName", {
  params = { { name = "object", type = "address" } },
  returns = "string, or nil, error",
  doc = "An object's path through its outers, such as `/Game/Maps/Level.Level.PersistentLevel.Pawn_1`.",
}, function(object)
  return result(native.unrealFullName(object))
end)

define(unreal, "classOf", {
  params = { { name = "object", type = "address" } },
  returns = "address or nil, or nil, error",
  doc = "An object's class.",
}, function(object)
  return many(native.unrealClassOf(object))
end)

define(unreal, "type", {
  params = { { name = "name", type = "string" } },
  returns = "address or nil, or nil, error",
  doc = "A class or struct by name, any case, without the U, A or F prefix.",
}, function(name)
  return many(native.unrealType(name))
end)

define(unreal, "types", {
  params = { { name = "filter", type = "string", optional = true } },
  returns = "list of `{ name, kind, address }`, or nil, error",
  doc = "Every class, struct and enum, sorted by name; with `filter`, the ones whose name contains it. `kind` is `class`, `struct` or `enum`.",
}, function(filter)
  return result(native.unrealTypes(filter))
end)

define(unreal, "super", {
  params = { { name = "type", type = "string|integer" } },
  returns = "address or nil, or nil, error",
  doc = "The parent of a class or struct.",
}, function(type)
  return many(native.unrealSuper(type))
end)

define(unreal, "size", {
  params = { { name = "type", type = "string|integer" } },
  returns = "integer or nil, or nil, error",
  doc = "The size of a class or struct in bytes.",
}, function(type)
  return many(native.unrealSize(type))
end)

define(unreal, "cppName", {
  params = { { name = "type", type = "string|integer" } },
  returns = "string, or nil, error",
  doc = "A type's C++ spelling: `F` for structs, `A` for actors, `U` for other classes.",
}, function(type)
  return result(native.unrealCppName(type))
end)

local FIELD_TEXT = "A field is `{ name, type, typeText, owner, offset, start, size, dim, flags, address, inner, metaClass, byteOffset, bitMask }`: `type` is the property class (`FloatProperty`), `typeText` the C++ type (`TArray<AActor*>`), `start` the offset of the first byte (a bitfield bool adds `byteOffset` and has `bitMask`), `inner` the struct, class or enum it refers to."

define(unreal, "fields", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "ownOnly", type = "boolean", optional = true },
  },
  returns = "list of fields, or nil, error",
  doc = "The reflected properties of a class or struct, its parents' included unless `ownOnly`, sorted by offset. " .. FIELD_TEXT,
}, function(type, ownOnly)
  return result(native.unrealFields(type, ownOnly == true))
end)

define(unreal, "field", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "name", type = "string" },
  },
  returns = "field, or nil, error",
  doc = "One property by name, any case, searched in the parents too; `ME-4203` when there is none. " .. FIELD_TEXT,
}, function(type, name)
  return result(native.unrealField(type, name))
end)

define(unreal, "offset", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "name", type = "string" },
  },
  returns = "symbol, offset, or nil, error",
)MLUA"),
            std::string_view(R"MLUA(  doc = "The offset symbol of a property (`Actor_RootComponent`) and its value. The symbol works in address expressions in every session, so records written with it keep working after a game update.",
}, function(type, name)
  return many(native.unrealSymbol(type, name))
end)

define(unreal, "functions", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "inherited", type = "boolean", optional = true },
  },
  returns = "list of `{ name, owner, address, flags, parameters }`, or nil, error",
  doc = "The UFunctions a class declares, and with `inherited` its parents' too. `parameters` are fields; the return value is the one with the return flag (`0x400` in `flags`).",
}, function(type, inherited)
  return result(native.unrealFunctions(type, inherited == true))
end)

define(unreal, "enum", {
  params = { { name = "enum", type = "string|integer" } },
  returns = "list of `{ name, value }`, or nil, error",
  doc = "The entries of an enum, by name or address.",
}, function(enum)
  return result(native.unrealEnum(enum))
end)

define(unreal, "instances", {
  params = {
    { name = "type", type = "string|integer" },
    { name = "limit", type = "integer", optional = true },
  },
  returns = "list of addresses, or nil, error",
  doc = "The live objects of a class and its subclasses, without the class default objects. One pass over the object array; `limit` stops early.",
}, function(type, limit)
  return result(native.unrealInstances(type, limit))
end)

define(unreal, "find", {
  params = { { name = "name", type = "string" } },
  returns = "address or nil, or nil, error",
  doc = "The first object with that name, or with that full name when `name` has a dot in it.",
}, function(name)
  return many(native.unrealFind(name))
end)

define(unreal, "owner", {
  params = { { name = "address", type = "address" } },
  returns = "object, class, or nil, error",
  doc = "The object an address belongs to and its class; `ME-4204` when no live object holds it.",
}, function(address)
  return many(native.unrealOwner(address))
end)

define(unreal, "whois", {
  params = { { name = "address", type = "address" } },
  returns = "string, or nil, error",
  doc = "One line about an address: the object, its class, the offset, the property path down to the innermost field and that field's type, such as `BP_Pawn_C_1 (ABP_Pawn_C) +130: RootComponent  USceneComponent*`.",
}, function(address)
  return result(native.unrealWhois(address))
end)

define(unreal, "locate", {
  params = { { name = "address", type = "address" } },
  returns = "`{ object, class, offset, path, field, within, struct }`, or nil, error",
  doc = "What `whois` says, as a table: `path` is the property path as text, `field` the innermost field, `within` the offset inside it, and `struct` the outermost struct property that starts exactly at the address.",
}, function(address)
  return result(native.unrealLocate(address))
end)

define(unreal, "engine", {
  returns = "address or nil, or nil, error",
  doc = "The GameEngine object (or the Engine of an editor build).",
}, function()
  return many(native.unrealEngine())
end)

define(unreal, "world", {
  returns = "address or nil, or nil, error",
  doc = "The world the engine's game viewport shows; nil while no level is loaded.",
}, function()
  return many(native.unrealWorld())
end)

define(unreal, "controller", {
  returns = "address or nil, or nil, error",
  doc = "The first local player's PlayerController.",
}, function()
  return many(native.unrealController())
end)

define(unreal, "pawn", {
  returns = "address or nil, or nil, error",
  doc = "The pawn the local PlayerController possesses.",
}, function()
  return many(native.unrealPawn())
end)

define(unreal, "offline", {
  returns = "boolean or nil, or nil, error",
  doc = "true when the world has no net driver or one without connections, false when it is connected or hosts remote players, nil when it cannot tell. Writes in an online session can get the account banned.",
}, function()
  return many(native.unrealIsOffline())
end)

define(unreal, "walk", {
  params = {
    { name = "object", type = "address" },
    { name = "steps", type = "table" },
    { name = "root", type = "string", optional = true },
  },
  returns = "address, expression, field, or nil, error",
  doc = "Follows properties by name from an object: `walk(manifold.unreal.pawn(), { \"RootComponent\", \"RelativeLocation\" })`. Object properties are followed, structs entered, and `Name[i]` picks a TArray element. Gives the address of the last step, an address expression from `root` (the object's address by default) with offset symbols, such as `[GameEngine+GameEngine_GameViewport]+...`, and the last field.",
}, function(object, steps, root)
  return many(native.unrealWalk(object, steps, root))
end)

define(unreal, "dump", {
  params = { { name = "type", type = "string|integer" } },
  returns = "list of lines, or nil, error",
  doc = "A class or struct as C++-like lines: its name, parent and size, then one line per property with offset, type, name and the class that declares it.",
}, function(type)
  return result(native.unrealDump(type))
end)

define(unreal, "claims", {
  params = { { name = "address", type = "any" } },
  returns = "boolean",
  doc = "Whether the tool names the address itself (a live object or a remembered array), for other dissect scripts that step aside for Unreal objects. Never raises, false before it is ready.",
}, function(address)
  return native.unrealClaims(address) == true
end)

local rtti = area("rtti", "C++ class names from the run-time type information of the opened process, built into the program. Every object of a C++ class with virtual functions starts with a pointer to its class's virtual function table, and in programs built with MSVC or clang-cl with RTTI on (the compiler's default) the slot in front of that table leads to the class's name and base classes. This area reads that in 64-bit and 32-bit processes; nothing is injected or called. Structure Dissect uses it to name structures and pointer fields and to embed member objects (Extensions > C++ RTTI turns that off), and `<Class>::vftable` and `<Class>::vftable@<Base>` symbols resolve in every session. Unreal Engine builds switch RTTI off and GCC builds use another format; there the lookups find nothing. Classes are given by name with their namespaces (`\"Game::Player\"`) or by decorated name (`\".?AVPlayer@Game@@\"`).")

define(rtti, "className", {
  params = { { name = "address", type = "address" } },
  returns = "string or nil, or nil, error",
  doc = "The class of the object whose virtual function table pointer is stored at the address; nil when there is none.",
}, function(address)
  return many(native.rttiClassName(address))
end)

define(rtti, "object", {
  params = { { name = "address", type = "address" } },
  returns = "`{ address, start, offset, class, decorated, vtable, module, viaVirtualBase }` or nil, or nil, error",
  doc = "The object whose virtual function table pointer is stored at the address. `start` is where the complete object starts, which is below the address when it holds a base class's table pointer; `viaVirtualBase` says the object starts with a virtual base table and was found through it.",
}, function(address)
  return many(native.rttiObject(address))
end)

define(rtti, "owner", {
  params = {
    { name = "address", type = "address" },
    { name = "window", type = "integer", optional = true },
  },
  returns = "the table `object` gives or nil, or nil, error",
  doc = "The nearest object at or before the address, searched back over `window` bytes (0x4000 by default); `offset` is the address's offset in it. A member object with virtual functions of its own counts as an object, because RTTI does not say how large a class is.",
}, function(address, window)
  return many(native.rttiOwner(address, window))
end)

define(rtti, "whois", {
  params = { { name = "address", type = "address" } },
  returns = "string, or nil, error",
  doc = "Several lines about the object at or before an address: the class, the object's start, the decorated name, the virtual function table, the module and the base classes with their offsets. `ME-4301` when no object is found.",
}, function(address)
  return result(native.rttiWhois(address))
end)

define(rtti, "bases", {
  params = { { name = "what", type = "string|integer" } },
  returns = "list of `{ name, decorated, depth, offset, virtual }`, or nil, error",
  doc = "A class and its base classes in the order of the hierarchy, the class itself first; `depth` is the level below it. `what` is a class name (a string is always a name, since names such as `Face` would also read as hex) or the address of an object. The offset of a virtual base is known only for an object and missing for a name.",
}, function(what)
  return result(native.rttiBases(what))
end)

define(rtti, "vtable", {
  params = {
    { name = "name", type = "string" },
    { name = "base", type = "string", optional = true },
  },
  returns = "address or nil, or nil, error",
  doc = "A class's virtual function table: the primary one, or the one for a base class part, given by the base's name or its offset in hex. The class is searched in the process's modules by name, without an index of the whole game; nil when it is not found.",
}, function(name, base)
  return many(native.rttiVTable(name, base))
end)

define(rtti, "vtables", {
  params = { { name = "name", type = "string" } },
  returns = "list of `{ address, offset, symbol, module }`, or nil, error",
  doc = "Every virtual function table of a class, ordered by the offset of the part it belongs to, with the symbol it is registered under.",
}, function(name)
  return result(native.rttiVTables(name))
end)

define(rtti, "instances", {
  params = {
    { name = "name", type = "string" },
    { name = "limit", type = "integer", optional = true },
  },
  returns = "list of addresses, or nil, error",
  doc = "The objects of exactly that class: one search of the process's writable memory for the class's primary table pointer, at most `limit` hits (100000 by default, 1000000 at most). Objects of derived classes have another table and are not found.",
}, function(name, limit)
  return result(native.rttiInstances(name, limit))
end)

define(rtti, "classes", {
  returns = "list of names, or nil, error",
  doc = "The classes met so far in this process, sorted. It is what the lookups have seen, not an index of the game.",
}, function()
  return result(native.rttiClasses())
end)

define(rtti, "demangle", {
  params = { { name = "decorated", type = "string" } },
  returns = "string",
  doc = "The readable name of a decorated type name: `.?AVPlayer@Game@@` gives `Game::Player`. Templates, nested and anonymous namespaces, local classes and lambdas are read; a name it cannot read comes back unchanged. Needs no process.",
}, function(decorated)
  return native.rttiDemangle(decorated)
end)

define(rtti, "mangle", {
  params = { { name = "name", type = "string" } },
  returns = "list of decorated names, or nil",
  doc = "The decorated names a plain class name can have, as a class and as a struct: `Game::Player` gives `.?AVPlayer@Game@@` and `.?AUPlayer@Game@@`. nil for a name with template arguments or other characters. Needs no process.",
}, function(name)
  return native.rttiMangle(name)
end)

define(rtti, "dissect", {
  params = { { name = "address", type = "address" } },
  returns = "true, or nil, error",
  doc = "Opens Structure Dissect on the object at or before the address, with a structure named after its class; the structure is made, labelled and given its embedded members when none of that name exists.",
}, function(address)
  return result(native.rttiDissect(address))
end)

define(rtti, "label", {
  params = {
    { name = "structure", type = "any" },
    { name = "address", type = "address" },
  },
  returns = "number of labelled fields, or nil, error",
)MLUA"),
            std::string_view(R"MLUA(  doc = "What Structure Dissect does after its guess, for a structure that already exists (a Cheat Engine structure object): pointer fields without a name of their own are named `vftable`, `vftable (Base)`, `<Class> vftable`, `<Class>*` or `vbtable`, member objects with a table of another class become embedded structures named after that class, and a structure laid over a virtual function table gets `virtual N`. 0 when the address is neither an object's start nor a table.",
}, function(structure, address)
  return result(native.rttiLabel(structure, address))
end)

define(rtti, "status", {
  returns = "`{ text, dissect, process, is64Bit, modules, classes, vtables }`",
  doc = "What is known about the opened process: `text` is the line the menu shows, `dissect` whether Structure Dissect uses RTTI, and the counts are what the lookups have seen. Without a process only `text` and `dissect` are there.",
}, function()
  return native.rttiStatus()
end)

define(rtti, "reset", {
  returns = "true",
  doc = "Forgets what was learned about the process and takes back the `::vftable` symbols it registered. The next lookup starts fresh.",
}, function()
  return native.rttiReset()
end)

define(rtti, "claims", {
  params = { { name = "address", type = "any" } },
  returns = "boolean",
  doc = "Whether Structure Dissect's RTTI stage names the address itself: an object or a virtual function table that neither the Unreal tool nor a Godot script claims. Never raises, false without a process or with the stage turned off.",
}, function(address)
  return native.rttiClaims(address) == true
end)

local speed = area("speed", "The speedhack: the opened process's clocks run faster or slower by a factor. Setting a speed loads a small library into the process and redirects its time functions (`QueryPerformanceCounter`, `GetTickCount`, `GetTickCount64`, `timeGetTime`) through it; sleeps and the wall clock stay as they are. A speed change never makes a clock jump or run backwards. `disable` goes back to normal speed and takes the hooks out when the clocks were never shifted; after a shift the hooks stay at normal speed so the game's time does not jump, and `remove` takes them out regardless. Works in 64-bit and 32-bit processes. If Manifold Engine ends while a speed is set, the process goes back to normal speed by itself.")

define(speed, "get", {
  returns = "number",
  doc = "The speed factor in use: 1 while the speedhack is off, 2 for double speed, 0.5 for half speed.",
}, function()
  return native.speedGet()
end)

define(speed, "set", {
  params = { { name = "speed", type = "number" } },
  returns = "true, or nil, error",
  doc = "Turns the speedhack on if needed and sets the factor, from 0 (time stands still) to 1000. `ME-1005` without a process, `ME-1019` when the time functions cannot be redirected.",
}, function(factor)
  return result(native.speedSet(factor))
end)

define(speed, "isEnabled", {
  returns = "boolean",
  doc = "Whether a speed is set.",
}, function()
  return native.speedIsEnabled() == true
end)

define(speed, "disable", {
  returns = "true, or nil, error",
  doc = "Goes back to normal speed. The hooks are taken out when the process's clocks still equal the real ones; otherwise they stay and pass time on at normal speed.",
}, function()
  return result(native.speedDisable())
end)

define(speed, "remove", {
  returns = "true, or nil, error",
  doc = "Takes the hooks out and restores the time functions byte for byte. After a speed other than 1 the process's clocks jump to the real time at that moment, backwards if the game ran faster.",
}, function()
  return result(native.speedRemove())
end)

define(speed, "status", {
  returns = "`{ enabled, speed, installed, clockShifted, hooks }`",
  doc = "The state of the speedhack: `installed` says whether the hooks are in the process, `clockShifted` whether its clocks differ from the real ones, and `hooks` lists `{ name, address }` for every redirected time function.",
}, function()
  return native.speedStatus()
end)

-- ∑ In a do block like manifold.record: the main chunk is close to Lua's limit of 200 locals.
do
  local signature = area("signature", "Byte signatures: a pattern for an address that matches nowhere else, so a script or an `aobscanmodule` finds the place again after the game is updated. `make` grows the pattern from the address one byte at a time, with the numbers that move between builds (addresses, jump distances, structure offsets, large constants) as wildcards, until no other place in the module (or the process) matches it. The defaults are the ones under Settings > Signatures; the options of a call override them for that call. `parse` reads signatures in the shapes other tools write, and `find` searches for one. Copy Signature and the Signature Maker in the memory viewer use the same functions.")

  local makeKeys = {
    scope = "string", codeOnly = "boolean", maskAddresses = "boolean", branches = "string", offsets = "string", immediates = "string",
    immediateThreshold = "integer", registers = "string", maxBytes = "integer", minBytes = "integer", stopAtFunctionEnd = "boolean", offset = "boolean",
    maxOffset = "integer", minSaving = "integer", wildcard = "string", spaces = "boolean", label = "string",
  }

  define(signature, "make", {
    params = {
      { name = "address", type = "address" },
      { name = "options", type = "table", optional = true, keys = makeKeys },
    },
    returns = "`{ pattern, mask, code, array, autoAssembler, address, start, offset, length, wildcards, instructions, module, scope, uniqueIn, verified, functionEnd, seconds, bytes }`, or nil, error",
    doc = "Makes the shortest pattern from the address that matches only there. `pattern` is it as an AoB (`\"48 8B 05 ?? ?? ?? ?? 8B 88\"`), `mask` the matching `x`/`?` string, `code`, `array` and `autoAssembler` the other ways to write it; `bytes` lists `{ value, wild, role }` per byte. With `options.offset` the pattern may start at an instruction up to `maxOffset` bytes before or after the address when that saves `minSaving` bytes: `start` is where it begins and `offset` is `address - start`, so the address is a scan hit plus `offset`. `options.scope` is `\"module\"` (unique in the address's module, like `aobscanmodule`) or `\"process\"`; `codeOnly` counts executable memory only. `branches` and `offsets` take `\"keep\"`, `\"long\"` (32-bit ones only) or `\"all\"`; `immediates` takes `\"keep\"`, `\"large\"` (from `immediateThreshold` up) or `\"all\"`; `maskAddresses` false keeps RIP-relative and absolute addresses. `registers` takes `\"keep\"`, `\"sib\"` (the base and index registers of memory operands, the default) or `\"all\"` (also ModRM bytes whose reg field is a register, never an opcode extension). `maxBytes` (up to 1024) and `minBytes` bound the length, `stopAtFunctionEnd` never goes past a ret, jmp or int3. `wildcard` (`\"??\"`, `\"?\"`, `\"**\"`, `\"*\"`), `spaces` and `label` shape the text. `functionEnd` is set when the pattern reaches past the end of the function. `ME-2018` when no pattern within `maxBytes` is unique; a module-wide search takes milliseconds, a process-wide one reads all memory.",
  }, function(address, options)
    options = options or EMPTY
    for _, key in ipairs({ "maxBytes", "minBytes", "maxOffset", "minSaving", "immediateThreshold" }) do
      if options[key] ~= nil and options[key] < 0 then
        misuse("manifold.signature.make: options." .. key .. " must not be negative")
      end
    end
    return result(native.signatureMake(address, options))
  end)

  define(signature, "parse", {
    params = { { name = "text", type = "string" } },
    returns = "`{ pattern, length, fixed, wildcards, partial, source, module, notes }`, or nil, error",
    doc = "Reads a signature as other tools write it: `\"48 8B ?? ??\"`, `\"48 8B ? ?\"`, `\"48 8B ** *\"`, `\"488B????\"`, the C string with its mask (`\"\\x48\\x8B\\x00\", \"xx?\"`), a C array (`{ 0x48, 0x8B }`), nibble wildcards (`\"4?\"`) or a whole `aobscanmodule(name,module,bytes)` line, with comment and header lines around it. `pattern` is it written as `manifold.memory.scan` takes it, `partial` counts the half-known bytes, `source` says which shape it was, `module` is the module of an `aobscanmodule` line, and `notes` lists what was read in a way that may surprise (a C string without a mask keeps its zeros). `ME-2003` when it does not read as a pattern.",
  }, function(text)
    return result(native.signatureParse(text))
  end)

  define(signature, "find", {
    params = {
      { name = "text", type = "string" },
      { name = "options", type = "table", optional = true, keys = { module = "string", executable = "boolean", limit = "integer" } },
    },
    returns = "list of integers, or nil, error",
    doc = "Reads a signature like `parse` and returns where it matches, in address order: in `options.module`, else in the module of an `aobscanmodule` line, else in the whole process. `options.executable` true searches code only; `options.limit` (1 to 100000, default 100) stops after that many hits. No hit is an empty list.",
  }, function(text, options)
    options = options or EMPTY
    if options.limit ~= nil and (options.limit < 1 or options.limit > 100000) then
      misuse("manifold.signature.find: options.limit must be 1 to 100000")
    end
    return result(native.signatureFind(text, options))
  end)
end

-- ∑ In a do block like manifold.signature: the main chunk is close to Lua's limit of 200 locals.
do
  local rebase = area("rebase", "Rebasing shows a module at another base, the way IDA's Rebase Program does: at the image base its PE header asks for (the addresses IDA and Ghidra show for the file, whatever ASLR picked this time), at 0 for RVAs, or at a base of your own. Only the addresses on screen change (the disassembler, the hex view, Copy and Go to Address); the process, the instructions and everything written stay at the real addresses, and symbol expressions keep reading real addresses. Rules are kept by module name in the settings, so they hold for the next start of the game. The memory viewer and Symbols and Modules > Modules use the same rules.")

  local function entryDoc(text)
    return text .. " An entry is `{ module, rule, base }`: `rule` is `\"image\"` or the chosen base, `base` the base the module is shown at now (nil while it is not loaded)."
  end

  define(rebase, "set", {
    params = {
      { name = "module", type = "string" },
      { name = "base", type = "integer|string" },
    },
    returns = "true",
    doc = "Shows the module at `base`: `\"image\"` for the image base in its PE header, or a number (0 shows RVAs). The module need not be loaded yet; the rule holds whenever a module of that name is. `clear` takes it back.",
  }, function(module, base)
    if module == "" then
      misuse("manifold.rebase.set: module must not be empty")
    end
    if type(base) == "string" and base ~= "image" then
      misuse(("manifold.rebase.set: base must be \"image\" or a number, got %q"):format(base))
    end
    if type(base) == "number" and base < 0 then
      misuse("manifold.rebase.set: base must not be negative")
    end
    return native.rebaseSet(module, base)
  end)

  define(rebase, "get", {
    params = { { name = "module", type = "string" } },
    returns = "entry, or nil",
    doc = entryDoc("The rebase of the module, or nil when it is shown where it is loaded."),
  }, function(module)
    return native.rebaseGet(module)
  end)

  define(rebase, "clear", {
    params = { { name = "module", type = "string" } },
    returns = "true",
    doc = "Shows the module at its real addresses again.",
  }, function(module)
    return native.rebaseSet(module, nil)
  end)

  define(rebase, "list", {
    returns = "list of entries",
    doc = entryDoc("Every rebased module, by name."),
  }, function()
    return native.rebaseList()
  end)

  define(rebase, "shown", {
    params = { { name = "address", type = "address" } },
)MLUA"),
            std::string_view(R"MLUA(    returns = "integer, or nil, error",
    doc = "The address as it is shown: rebased when it lies in a rebased module, else the address itself.",
  }, function(address)
    return result(native.rebaseShown(address))
  end)

  define(rebase, "real", {
    params = { { name = "shown", type = "integer" } },
    returns = "integer, or nil, error",
    doc = "Where a shown address really is, such as an address copied from IDA: the real address when it lies in a rebased module as shown, else the number itself. With rebased modules shown over each other the first in the module list wins.",
  }, function(shown)
    return result(native.rebaseReal(shown))
  end)

  define(rebase, "imageBase", {
    params = { { name = "module", type = "string" } },
    returns = "integer, or nil, error",
    doc = "The image base in the PE header of a loaded module: where it asks to be loaded, and where `\"image\"` shows it. `ME-1016` when no module has the name.",
  }, function(module)
    return result(native.rebaseImageBase(module))
  end)
end

-- ∑ In a do block like manifold.signature: the main chunk is close to Lua's limit of 200 locals.
do
  local codefilter = area("codefilter", "The code filter: finds code by what it does instead of by a value. `prepare` collects the function starts of the game's modules (the unwind table of 64-bit modules, the exports and checked call targets), and every round puts a one-time int3 on each one that is left, inside the process through the agent library: the first thread that runs into one puts the byte back and runs on. A `\"filterOut\"` round drops what ran during it (let the game run without doing the thing), a `\"keepOnly\"` round keeps only what ran (do the thing once), so a few rounds leave the code behind an action. While a round runs every read through Manifold shows the original bytes, and a write over an armed byte takes its int3 back first. Tools > Code Filter is the same filter: the window and these functions share one state. Games that check their own code or run an anti-cheat notice the int3s.")

  define(codefilter, "prepare", {
    params = {
      { name = "options", type = "table", optional = true, keys = { modules = "table", unwind = "boolean", exports = "boolean", pointers = "boolean", calls = "boolean" } },
    },
    returns = "`{ total, seconds, modules }`, or nil, error",
    doc = "Collects the candidates and starts a new filter (the rounds and the undo steps before are gone). `options.modules` lists module names, by default every module that is not part of Windows or Manifold's own library; `unwind`, `exports` and `pointers` (all on by default) and `calls` (on by default for 32-bit, from Settings for 64-bit) choose the sources; `pointers` takes the functions that are only reached through a pointer (virtual methods from their vtables, callbacks, function tables), from the module's relocations and, with `calls`, the code's own address references. `modules` in the result lists `{ name, functions, unwind, exports, calls, pointers, imported, rejected }` per module. Call targets take a code dissect of each module, which takes a while in a large game. `ME-1016` for a module that is not loaded, `ME-1020` when nothing can be collected.",
  }, function(options)
    return result(native.codefilterPrepare(options or EMPTY))
  end)

  define(codefilter, "start", {
    params = { { name = "kind", type = "string" } },
    returns = "true, or nil, error",
    doc = "Starts a round on what is left: `\"filterOut\"` or `\"keepOnly\"`. Addresses the agent cannot arm (system code, an int3 already there, a page that cannot be written) are dropped from the list. `ME-1020` when `prepare` was not called, a round already runs or nothing is left.",
  }, function(kind)
    if kind ~= "filterOut" and kind ~= "keepOnly" then
      misuse("manifold.codefilter.start: kind must be \"filterOut\" or \"keepOnly\"")
    end
    return result(native.codefilterStart(kind))
  end)

  define(codefilter, "stop", {
    returns = "`{ kind, before, after, hits, skipped, seconds }`, or nil, error",
    doc = "Ends the round and applies it: `filterOut` drops every candidate that ran, `keepOnly` keeps only those. Returns the round's record. `ME-1020` when no round runs.",
  }, function()
    return result(native.codefilterStop())
  end)

  define(codefilter, "cancel", {
    returns = "true, or nil, error",
    doc = "Ends the round without changing the list. Nothing happens when no round runs.",
  }, function()
    return result(native.codefilterCancel())
  end)

  define(codefilter, "undo", {
    returns = "boolean",
    doc = "Goes back one step (a round or a refine), up to 32 steps. False when there is nothing to undo or a round runs.",
  }, function()
    return native.codefilterUndo() == true
  end)

  define(codefilter, "reset", {
    returns = "true",
    doc = "Ends a running round and forgets the candidates, the rounds and the undo steps.",
  }, function()
    return native.codefilterReset()
  end)

  define(codefilter, "refine", {
    params = { { name = "maxFunctions", type = "integer", optional = true } },
    returns = "integer, or nil, error",
    doc = "Splits every function that is left into its basic blocks, from its flow graph, so the next rounds show which part of it ran; blocks have the source `\"block\"` and their `function`. Returns how many candidates there are now. `ME-1020` when more than `maxFunctions` (default 256) functions are left or every candidate is a block already.",
  }, function(maxFunctions)
    if maxFunctions ~= nil and (maxFunctions < 1 or maxFunctions > 4096) then
      misuse("manifold.codefilter.refine: maxFunctions must be 1 to 4096")
    end
    return result(native.codefilterRefine(maxFunctions))
  end)

  define(codefilter, "import", {
    params = {
      { name = "addresses", type = "string|table" },
      { name = "options", type = "table", optional = true, keys = { add = "boolean" } },
    },
    returns = "`{ imported, lines, duplicates, rejected, total, problems, seconds }`, or nil, error",
    doc = "Takes the candidates from a list of addresses instead of collecting them, like Filter > Import Addresses: a text with one address per line (hex, `module+offset`, a symbol or any address expression; empty lines and lines starting with `#`, `;` or `//` are skipped; a line with more words uses its first), or a table of addresses and texts. An address must be executable code where an instruction starts; code outside a module (JIT code) is fine. By default the list replaces the candidates and starts a new filter; `options.add` adds it to the collected list instead, which only works before the first round. `problems` holds the first 20 rejected lines with the reason. `ME-1020` when no line is an address in code, or `add` after a round.",
  }, function(addresses, options)
    local text = addresses
    if type(addresses) == "table" then
      local lines = {}
      for i, value in ipairs(addresses) do
        if math.type(value) == "integer" then
          lines[i] = string.format("%X", value)
        elseif type(value) == "string" then
          lines[i] = value
        else
          misuse("manifold.codefilter.import: addresses[" .. i .. "] must be an integer or a string")
        end
      end
      text = table.concat(lines, "\n")
    end
    return result(native.codefilterImport(text, options ~= nil and options.add == true))
  end)

  define(codefilter, "export", {
    returns = "string",
    doc = "What is left as text for `import` or Filter > Import Addresses: a comment header, then one `module+offset` per line, which still points at the same code after the game restarted.",
  }, function()
    return native.codefilterExport()
  end)

  define(codefilter, "candidates", {
    params = { { name = "limit", type = "integer", optional = true } },
    returns = "list of `{ address, name, function, sources }`",
    doc = "What is left, in address order, at most `limit` (default 1000, up to 1000000). `name` is the address as the disassembler names it, `sources` lists where it came from (`\"unwind\"`, `\"export\"`, `\"call\"`, `\"pointer\"`, `\"import\"`, `\"block\"`), `function` is set for blocks.",
  }, function(limit)
    if limit ~= nil and (limit < 1 or limit > 1000000) then
      misuse("manifold.codefilter.candidates: limit must be 1 to 1000000")
    end
    return native.codefilterCandidates(limit)
  end)

  define(codefilter, "status", {
    returns = "`{ prepared, running, kind, remaining, total, liveHits, seconds }`",
    doc = "The filter's state: whether candidates are collected, whether a round runs and which, how many are left of how many collected, and for a running round how many candidates ran so far and for how long.",
  }, function()
    return native.codefilterStatus()
  end)

  define(codefilter, "history", {
    returns = "list of `{ kind, before, after, hits, skipped, seconds }`",
    doc = "The steps since `prepare`, oldest first: rounds (`\"filterOut\"`, `\"keepOnly\"`) and refines (`\"refine\"`).",
  }, function()
    return native.codefilterHistory()
  end)
end

-- ∑ In a do block like manifold.codefilter: the main chunk is close to Lua's limit of 200 locals.
do
  local crash = area("crash", "The crash watch: finds the script behind a crash of the game. Every Auto Assembler run that changes the game notes what it put there (its allocations and the bytes it patched), and once a script has written code the agent library notes every exception that ends a process, in memory Manifold Engine owns. When the game ends with a crash, the last exception is matched against the scripts: in a script's code, in or right behind its patch, in its memory after it was switched off, or with its code on the stack. The result goes into the log and a notification, and `last` returns it. Settings: [Crashes] Watch.")

  define(crash, "last", {
    returns = "`{ process, headline, details, exitCode, exact, watched, code, address, accessed, thread, secondsBeforeEnd, scripts, switchedOn, lastSwitched, lastSwitchedSeconds }`, or nil",
    doc = "The report of the last crash in this session, or `nil` when none crashed. `scripts` lists the scripts involved, most likely first, as `{ script, recordId, link, where, address }`; `link` is `\"code\"` (the failing instruction is in its code), `\"patch\"` (in bytes it patched), `\"behindPatch\"` (right behind its patch), `\"freed\"` (in its memory after it was switched off), `\"unbalanced\"` (its hook leaves the stack unbalanced, from the Auto Assembler's stack check), `\"nearPatch\"` (the game failed shortly behind its patch),`\"accessed\"` (the failing access reached its memory), `\"stack\"` (its code is on the stack) or `\"earlier\"` (an exception in its code shortly before). `exact` is true when the exit code is the noted exception's code. `code`, `address` (the instruction) and `accessed` (an access violation's address) are missing when no exception was noted.",
  }, function()
    return native.crashLast()
  end)

  define(crash, "watching", {
    returns = "boolean",
    doc = "Whether the agent watches the open process for crashes.",
  }, function()
    return native.crashWatching() == true
  end)

  define(crash, "start", {
    returns = "true, or nil, error",
    doc = "Starts the crash watch in the open process now, without waiting for a script to write code. Loads the agent library into the game. `ME-1021` when it cannot.",
  }, function()
    return result(native.crashStart())
  end)

  define(crash, "scripts", {
    returns = "list of `{ script, recordId, on, ranges, warnings }`",
)MLUA"),
            std::string_view(R"MLUA(    doc = "What the scripts put into the open process, as the crash watch matches it: per run that changed the game its label (the record's description, or for a script run from Lua the record whose Lua ran it), record id, whether it is still on, `ranges` of `{ address, size, kind, name }` with `kind` `\"alloc\"` or `\"patch\"`, and `warnings` of `{ line, text }`: what the Auto Assembler's stack check found, such as a push without its pop before the jump back.",
  }, function()
    return native.crashScripts()
  end)
end

local dotnet = area("dotnet", "The .NET runtime of a .NET game (.NET Framework or .NET 5 and later, 64-bit and 32-bit), read from outside the game by Manifold's .NET helper through the runtime's own data access library: nothing runs inside the game. `load` reads the classes, as Extensions > .NET does; the other functions need that and return `nil` and an `ME-4104` error before it. A class is named `\"Namespace.Class\"`, a nested one `\"Namespace.Outer+Inner\"`, and `\"Class\"` alone works when it is unique. A field or method of a class is written `\"Class:Name\"`, which also resolves as an address everywhere. Objects move when the garbage collector runs, so an object address is good for the moment; a path from a static field (`value`, `address`) finds the object again each time. Only classes the runtime has loaded are listed, and only methods it has compiled have an address.")

define(dotnet, "load", {
  returns = "true, or nil, error",
  doc = "Starts the .NET helper and reads the classes of the opened process's .NET runtime, unless that is done already. Takes a moment for a large game. `ME-4101` when the process has no .NET runtime, `ME-4104` when a Mono or IL2CPP runtime is loaded instead, `ME-4103` when the helper fails.",
}, function()
  return result(native.dotnetLoad())
end)

define(dotnet, "unload", {
  returns = "true",
  doc = "Ends the .NET helper and forgets the classes. Nothing happens when no .NET runtime is loaded.",
}, function()
  return native.dotnetUnload()
end)

define(dotnet, "isLoaded", {
  returns = "boolean",
  doc = "Whether a .NET runtime is loaded for the opened process.",
}, function()
  return native.dotnetIsLoaded() == true
end)

define(dotnet, "status", {
  returns = "`{ loaded, detected, runtime, module, classes, pointerSize, processId, generation }`",
  doc = "Where the .NET reader stands: `detected` says the opened process has a .NET runtime at all, `loaded` that its classes are read. `runtime` (such as `\".NET Framework 4.8.9363.0\"`), `module`, `classes`, `pointerSize`, `processId` and `generation` are there once it is loaded. `processId` is the game's process id; `generation` grows each time the classes are read again or forgotten. Together they tell a restarted game, which has the same module and often the same class count, from the one before: a cache of class handles keyed on them is never used for the wrong game.",
}, function()
  return native.dotnetStatus()
end)

define(dotnet, "modules", {
  returns = "list of `{ handle, name, path, base, size, dynamic }`, or nil, error",
  doc = "The loaded assemblies' modules: the runtime's handle for each, its name without the extension, its file, and where its image is. `dynamic` marks a module made at run time.",
}, function()
  return result(native.dotnetModules())
end)

define(dotnet, "classes", {
  params = { { name = "filter", type = "string", optional = true } },
  returns = "list of `{ name, namespace, image, handle, module, token, generic }`, or nil, error",
  doc = "The loaded classes, or those whose full name contains `filter` (ignoring case). `handle` is the class's method table, the pointer every object of it starts with; `module` and `token` say where it is defined.",
}, function(filter)
  return result(native.dotnetClasses(filter))
end)

define(dotnet, "class", {
  params = { { name = "name", type = "string" } },
  returns = "`{ name, namespace, image, parent, handle, module, token, generic, fields, statics, methods }` or nil, or nil, error",
  doc = "A class with its members. `fields` lists the instance fields declared by the class (not its parents') as `{ name, type, typeCode, offset, valueSize, typeHandle }`, the offset counted from the object's start; `statics` lists the static fields as `{ name, type, typeCode, address }`, without `address` while the runtime has made no storage for it; `methods` lists `{ name, address, handle, token, params, static }`, `address` only for a method the runtime has compiled. nil when no class has the name.",
}, function(name)
  return many(native.dotnetClass(name))
end)

define(dotnet, "classAt", {
  params = { { name = "address", type = "address" } },
  returns = "`{ class, address, offset }` or nil, or nil, error",
  doc = "The object an address is in: its class, where it starts and the address's offset in it. An address within a few KB after an object's start finds it, the way Structure Dissect does. nil when no object is there.",
}, function(address)
  return many(native.dotnetClassAt(address))
end)

define(dotnet, "object", {
  params = { { name = "address", type = "address" } },
  returns = "`{ address, class, handle, kind, size, length, elementClass, elementType, elementSize, data, rank }` or nil, or nil, error",
  doc = "What the runtime says about the object that starts at the address. `kind` is `object`, `string`, `array` or `free` (unused heap space); `length` is there for a string (its characters) and an array (its elements), and an array also has its element class, element type code, element size, `data` (the first element) and `rank`. nil when no object starts there.",
}, function(address)
  return many(native.dotnetObject(address))
end)

define(dotnet, "fields", {
  params = { { name = "address", type = "address" } },
  returns = "list of `{ name, type, typeCode, offset, valueSize, typeHandle }`, or nil, error",
  doc = "The instance fields of the object at the address, its parents' included, sorted by offset. `ME-4105` when no .NET object is there.",
}, function(address)
  return result(native.dotnetFields(address))
end)

define(dotnet, "methodAt", {
  params = { { name = "address", type = "address" } },
  returns = "`{ name, start, size }` or nil, or nil, error",
  doc = "The compiled method whose code holds the address, as `\"Namespace.Class:Method\"`, with its code's start and size. nil for an address in no managed code.",
}, function(address)
  return many(native.dotnetMethodAt(address))
end)

define(dotnet, "methodAddress", {
  params = {
    { name = "class", type = "string" },
    { name = "method", type = "string" },
  },
  returns = "address or nil, or nil, error",
  doc = "The code of a method once the runtime has compiled it, the same address `\"Class:Method\"` resolves to; with overloads, the first one that has code. nil when no such method has run yet.",
}, function(class, method)
  return many(native.dotnetMethodAddress(class, method))
end)

define(dotnet, "staticAddress", {
  params = {
    { name = "class", type = "string" },
    { name = "field", type = "string" },
  },
  returns = "address or nil, or nil, error",
  doc = "Where a static field's value is, the same address `\"Class:Field\"` resolves to. A static field of a reference type holds a reference there, a static struct a reference to a boxed copy. nil when the class has no such field or no storage for it yet.",
}, function(class, field)
  return many(native.dotnetStaticAddress(class, field))
end)

define(dotnet, "fieldOffset", {
  params = {
    { name = "class", type = "string" },
    { name = "field", type = "string" },
  },
  returns = "integer or nil, or nil, error",
  doc = "The offset of an instance field from the start of an object of the class, inherited fields included. An auto-property's backing field is also found by the property's name. nil when there is no such field.",
}, function(class, field)
  return many(native.dotnetFieldOffset(class, field))
end)

define(dotnet, "value", {
  params = {
    { name = "base", type = "any" },
    { name = "path", type = "string", optional = true },
  },
  returns = "number, boolean, string, address or nil, or nil, error",
  doc = "Reads a value by following a path. `base` is an object address or a static field as `\"Class:Field\"`; `path` names fields by name and array elements by index, as in `\"Players[0].statLife\"` or `\".Inventory[2].Name\"`. A number, a bool, a char's code and a string's text come back as Lua values, another object as its address, null as nil, an inline struct as its address. A step that does not exist, or a null on the way, gives `ME-4105`. The path is followed again on every call, so it keeps working while the garbage collector moves objects.",
}, function(base, path)
  return many(native.dotnetValue(base, path))
end)

define(dotnet, "address", {
  params = {
    { name = "base", type = "any" },
    { name = "path", type = "string", optional = true },
  },
  returns = "address, or nil, error",
  doc = "Where the value a path reaches is stored, for `manifold.memory.read` and `write` or the address list; the path works as for `value`. Without a path it is the base itself (the object, or the static field's storage). Good until the garbage collector moves the object; follow the path again for the next use.",
}, function(base, path)
  return result(native.dotnetAddress(base, path))
end)

define(dotnet, "text", {
  params = { { name = "address", type = "address" } },
  returns = "string or nil, or nil, error",
  doc = "The text of the `System.String` object at the address, at most a million characters; nil when no string is there.",
}, function(address)
  return many(native.dotnetText(address))
end)

define(dotnet, "heap", {
  returns = "`{ objects, bytes, segments, seconds, types }`, or nil, error",
  doc = "Walks the whole GC heap and counts the objects per type: `types` lists `{ name, handle, count, bytes }`, most bytes first. The game keeps running and the walk takes a few seconds for a large heap, during which the script waits.",
}, function()
  return result(native.dotnetHeap())
end)

define(dotnet, "instances", {
  params = {
    { name = "class", type = "string|integer" },
    { name = "limit", type = "integer", optional = true },
  },
  returns = "list of addresses, or nil, error",
  doc = "The objects of exactly one class on the GC heap (objects of derived classes have another class), at most `limit` (100000 by default, 1000000 at most). The class is given by name or by its handle. Walks the whole heap like `heap`; an object found here may already be dead and not collected yet.",
}, function(class, limit)
  return result(native.dotnetInstances(class, limit))
end)

local doc = area("doc", "The documentation behind completion, signature help, hover tooltips and the docs panel. Manifold, Cheat Engine and Lua functions are documented already. A module documents its own functions with `register`, or writes LuaLS annotations (`---@param name type`) above them and calls `registerModule`. A script never replaces Manifold's own documentation.")

define(doc, "register", {
  params = {
    { name = "target", type = "function|string" },
    { name = "spec", type = "table", keys = DOC_SPEC },
  },
  returns = "true, or nil, error",
  doc = "Documents a function, or a name such as `\"MyModule.run\"`. `spec.doc` is the text, `spec.params` a list of `{ name, type, optional, doc, oneOf }`, `spec.returns` what comes back and `spec.overloads` more `{ params, returns }`. A function without `spec.path` is documented wherever the help reaches it.",
}, function(target, spec)
  return docResult(native.docRegister(target, spec))
end)

define(doc, "registerModule", {
  params = {
    { name = "path", type = "string" },
    { name = "module", type = "table" },
    { name = "options", type = "table", optional = true, keys = { doc = "string" } },
  },
  returns = "number of registered functions, or nil, error",
)MLUA"),
            std::string_view(R"MLUA(  doc = "Documents every function of a module table under `path` (such as `\"ManifoldGodot\"`), from the LuaLS annotations above each function, or its parameter names when it has none. `options.doc` describes the module. Call it again after adding functions.",
}, function(path, module, options)
  return docResult(native.docRegisterModule(path, module, options))
end)

define(doc, "inherit", {
  params = {
    { name = "target", type = "function" },
    { name = "source", type = "function|string" },
  },
  returns = "true, or nil, error",
  doc = "Gives a wrapper the documentation of what it wraps, so a `function(...)` wrapper keeps the parameters and text of the original in the help.",
}, function(target, source)
  return docResult(native.docInherit(target, source))
end)

define(doc, "unregister", {
  params = { { name = "target", type = "function|string" } },
  returns = "true, or nil, error",
  doc = "Removes documentation that a script registered. Manifold's, Cheat Engine's and Lua's own documentation stays.",
}, function(target)
  return docResult(native.docUnregister(target))
end)

define(doc, "entry", {
  params = { { name = "target", type = "function|string" } },
  returns = "`{ path, name, kind, origin, summary, text, signatures, … }`, or nil, error",
  doc = "The documentation of a function or name, as the help shows it. `signatures` is a list of `{ text, params, returns }`; each param is `{ name, type, text, optional, variadic, choices }`. Without documentation the error is `ME-7008`.",
}, function(target)
  return docResult(native.docEntry(target))
end)

define(doc, "find", {
  params = {
    { name = "query", type = "string" },
    { name = "options", type = "table", optional = true, keys = { limit = "integer", kind = "string" } },
  },
  returns = "list of `{ path, kind, summary }`",
  doc = "Searches names and texts, best matches first. `options.limit` (default 50) caps the list; `options.kind` keeps one kind, such as `\"function\"` or `\"table\"`.",
}, function(query, options)
  options = options or EMPTY
  if options.limit ~= nil and options.limit < 0 then
    misuse("manifold.doc.find: options.limit must be 0 or more")
  end
  return docResult(native.docFind(query, options.limit or 50, options.kind or ""))
end)

define(doc, "annotations", {
  params = { { name = "fn", type = "function" } },
  returns = "`{ … }` like `entry`, or nil, error",
  doc = "Reads the LuaLS annotations above a function's definition without registering anything.",
}, function(fn)
  return docResult(native.docAnnotations(fn))
end)

define(doc, "markdown", {
  params = { { name = "prefix", type = "string" } },
  returns = "string, or nil, error",
  doc = "Markdown tables of everything documented under `prefix`, the way Docs/MANIFOLD-API.md is written.",
}, function(prefix)
  return docResult(native.docMarkdown(prefix))
end)

constant("apiVersion", API_VERSION, "The API version, an integer. It grows when functions are added; names keep their meaning.")
constant("version", native.version(), "The program version, such as `\"1.0.0\"`.", "string")

do
  local path = "manifold.has"
  local spec = {
    params = { { name = "name", type = "string" } },
    returns = "boolean",
    doc = "Whether the API has a name, such as `\"log.channel\"`, `\"memory\"` or `\"apiVersion\"`. Use it instead of reading a name that may not exist, which raises.",
  }
  bind(manifold, path, rootMembers, "has", spec, function(name)
    local short = name:gsub("^manifold%.", "")
    if entries[short] or short == "has" then
      return true
    end
    for _, section in ipairs(areas) do
      if section.name == short then
        return true
      end
    end
    return false
  end)
end

local ui = area("ui", "Windows, dialogs, notices, menu entries, toolbar buttons and status entries that Manifold draws itself, in its own look. A script describes what it wants (a window with a row of buttons, a table, a canvas) and keeps the handles it gets back; Manifold draws them every frame and calls the script's handlers when something happens. Colours are theme names (`accent`, `warning`, `muted`, ...), icons are Font Awesome names (`map-marker-alt`, `play`, ...). Everything a script made belongs to that script: when the same script runs again, its old windows, entries, notices and dialogs go away first.")

local function subarea(parent, name, summary)
  local path = parent.path .. "." .. name
  local proxy, members = strict(path)
  local section = { path = path, name = name, proxy = proxy, members = members }
  rawset(parent.members, name, proxy)
  native.docDefine(nil, path, { kind = "table", doc = summary })
  return section
end

local UI_COLORS = {
  "accent", "accentHovered", "success", "warning", "error", "text", "label", "muted", "changed",
  "page", "card", "cardBorder", "surface", "border", "rowSeparator", "frame", "button", "popup", "sidebar", "titleBar",
  "addressText", "bytesText", "codeKeyword", "codeRegister", "codeNumber", "codeString", "codeComment", "codeUserComment", "codeSection", "codeLabel",
  "flowJump", "flowCall", "flowReturn", "breakpoint", "currentLine", "shadow",
}
local UI_COLOR_SET = {}
for _, name in ipairs(UI_COLORS) do
  UI_COLOR_SET[name:lower()] = true
end

local UI_HANDLER_BITS = { onClick = 1, onChange = 2, onSelect = 4, onActivate = 8, onMenu = 16, onHover = 32, onSubmit = 64, onClose = 128, onView = 256, onKey = 512 }
local UI_EVENTS = { click = "onClick", change = "onChange", select = "onSelect", activate = "onActivate", menu = "onMenu", hover = "onHover", submit = "onSubmit", close = "onClose", view = "onView", key = "onKey", reload = "onFormReload" }

local COMMON_KEYS = { visible = "boolean", enabled = "boolean", tooltip = "string", width = "number", height = "number", minWidth = "number", maxWidth = "number" }

local function withCommon(keys)
  local all = {}
  for key, kind in pairs(COMMON_KEYS) do
    all[key] = kind
  end
  for key, kind in pairs(keys) do
    all[key] = kind
  end
  return all
end

local WIDGET_KEYS = {
  row = withCommon({ align = "string", gap = "number", wrap = "boolean" }),
  column = withCommon({ gap = "number", scroll = "boolean", labelPosition = "string" }),
  grid = withCommon({ columns = "integer" }),
  section = withCommon({ title = "string", icon = "string", badge = "string", collapsible = "boolean", open = "boolean", gap = "number", labelPosition = "string" }),
  tabs = withCommon({}),
  tab = withCommon({ title = "string", icon = "string", gap = "number", scroll = "boolean", labelPosition = "string" }),
  split = withCommon({ vertical = "boolean", ratio = "number", labelPosition = "string" }),
  text = withCommon({ text = "string", color = "string", wrap = "boolean", mono = "boolean", align = "string", size = "number", bold = "boolean", overflow = "string" }),
  heading = withCommon({ text = "string", icon = "string", color = "string", align = "string", size = "number", overflow = "string" }),
  separator = withCommon({ text = "string" }),
  spacer = withCommon({}),
  button = withCommon({ text = "string", icon = "string", style = "string", shortcut = "string", hotkey = "string", confirm = "string", onClick = "function" }),
  checkbox = withCommon({ label = "string", value = "boolean", style = "string", hotkey = "string", onChange = "function" }),
  input = withCommon({ label = "string", value = "string", hint = "string", multiline = "boolean", password = "boolean", readOnly = "boolean", maxLength = "integer", focus = "boolean", labelPosition = "string", onChange = "function", onSubmit = "function" }),
  number = withCommon({ label = "string", value = "number", min = "number", max = "number", step = "number", integer = "boolean", hex = "boolean", format = "string", focus = "boolean", labelPosition = "string", hotkeyIncrease = "string", hotkeyDecrease = "string", hotkeyStep = "number", onChange = "function" }),
  slider = withCommon({ label = "string", value = "number", min = "number", max = "number", step = "number", integer = "boolean", format = "string", labelPosition = "string", hotkeyIncrease = "string", hotkeyDecrease = "string", hotkeyStep = "number", onChange = "function" }),
  combo = withCommon({ label = "string", items = "table", value = "choice", multi = "boolean", hint = "string", labelPosition = "string", hotkeyNext = "string", hotkeyPrevious = "string", onChange = "function" }),
  radio = withCommon({ label = "string", items = "table", value = "integer", horizontal = "boolean", labelPosition = "string", hotkeyNext = "string", hotkeyPrevious = "string", onChange = "function" }),
  color = withCommon({ label = "string", value = "string", alpha = "boolean", labelPosition = "string", onChange = "function" }),
  table = withCommon({ columns = "table", rows = "table", value = "integer", menu = "table", onSelect = "function", onActivate = "function", onMenu = "function" }),
  tree = withCommon({ items = "table", value = "string", menu = "table", onSelect = "function", onActivate = "function", onMenu = "function" }),
  progress = withCommon({ value = "number", text = "string" }),
  image = withCommon({ image = "table" }),
  canvas = withCommon({ background = "string", zoom = "boolean", menu = "table", onClick = "function", onHover = "function", onMenu = "function", onView = "function", onKey = "function" }),
}
local WIDGET_KINDS = {}
for kind in pairs(WIDGET_KEYS) do
  WIDGET_KINDS[#WIDGET_KINDS + 1] = kind
end
table.sort(WIDGET_KINDS)
local CONTAINERS = { row = true, column = true, grid = true, section = true, tabs = true, tab = true, split = true }
local BUTTON_STYLES = { default = true, accent = true, danger = true }
local ROW_ALIGNS = { left = true, center = true, right = true }

local DRAW_KEYS = {
  line = { kind = true, x = true, y = true, x2 = true, y2 = true, color = true, thickness = true },
  rect = { kind = true, x = true, y = true, width = true, height = true, x2 = true, y2 = true, color = true, filled = true, rounding = true, thickness = true },
  circle = { kind = true, x = true, y = true, radius = true, color = true, filled = true, thickness = true },
  polygon = { kind = true, points = true, color = true, filled = true, thickness = true },
  text = { kind = true, x = true, y = true, text = true, color = true, size = true },
  image = { kind = true, image = true, x = true, y = true, width = true, height = true, x2 = true, y2 = true },
}

local uiEntries = {}
local uiOwned = {}
local imageIds = setmetatable({}, { __mode = "k" })

-- ∑ a live reload runs from the program, not from the script: what it makes
-- (modal windows, pictures) belongs to the form's script all the same, so
-- running the script again removes it. ownerSource(true, source) makes
-- `source` the owner until ownerSource(true, nil) (see reloadForm). The
-- override lives in a block: the main chunk is at Lua's 200 locals.
local ownerSource
do
  local override
  ownerSource = function(set, owner)
    if set then
      override = owner
      return
    end
    if override then
      return override
    end
    local info = callerFrame()
    return info and info.source or ""
  end
end

local function remember(owner, key, remove)
  local list = uiOwned[owner]
  if not list then
    list = {}
    uiOwned[owner] = list
  end
  list[key] = remove
end

local function forget(owner, key)
  local list = uiOwned[owner]
  if list then
    list[key] = nil
  end
end

local function gone(what)
  return nil, makeError("ME-7010", "The " .. what .. " is gone")
end

local function checkColor(path, name, value)
  if value == nil then
    return
  end
  if not UI_COLOR_SET[value:lower()] and not value:match("^#%x%x%x%x%x%x$") and not value:match("^#%x%x%x%x%x%x%x%x$") then
    misuse(("%s: %s %q is no theme colour (%s) and no #RRGGBB or #RRGGBBAA"):format(path, name, value, table.concat(UI_COLORS, ", ")))
  end
end

local function checkIcon(path, value)
  if value ~= nil and value ~= "" and not native.uiIsIcon(value) then
)MLUA"),
            std::string_view(R"MLUA(    local near = native.uiIcons(value:sub(1, math.min(#value, 4)))
    local hint = #near > 0 and (" (similar: " .. table.concat(near, ", ", 1, math.min(#near, 6)) .. ")") or ""
    misuse(("%s: icon %q is no Font Awesome icon%s; manifold.ui.theme.icons lists them"):format(path, value, hint))
  end
end

local function checkOptions(path, keys, options)
  if options == nil then
    return {}
  end
  if type(options) ~= "table" then
    misuse(("%s: options must be a table, got %s"):format(path, kindOf(options)))
  end
  local names = nil
  for key, value in pairs(options) do
    local kind = type(key) == "string" and keys[key]
    if not kind then
      names = names or table.concat(sortedNames(keys), ", ")
      misuse(("%s: options.%s is not an option (known options: %s)"):format(path, tostring(key), names))
    end
    if not CHECKS[kind](value) then
      misuse(("%s: options.%s must be %s, got %s"):format(path, key, NOUNS[kind], kindOf(value)))
    end
  end
  checkColor(path, "options.color", options.color)
  checkColor(path, "options.background", options.background)
  checkIcon(path, options.icon)
  if options.style ~= nil and keys == WIDGET_KEYS.checkbox then
    if options.style ~= "checkbox" and options.style ~= "switch" then
      misuse(('%s: options.style must be "checkbox" or "switch", got %q'):format(path, options.style))
    end
  elseif options.style ~= nil and not BUTTON_STYLES[options.style] then
    misuse(('%s: options.style must be one of "default", "accent", "danger", got %q'):format(path, options.style))
  end
  for _, key in ipairs({ "hotkey", "hotkeyIncrease", "hotkeyDecrease", "hotkeyNext", "hotkeyPrevious" }) do
    local text = options[key]
    if text ~= nil and text ~= "" and not native.uiHotkeyText(text) then
      misuse(('%s: options.%s %q names no key; write it like "F1", "Ctrl+F2" or "Shift+Num 1"'):format(path, key, text))
    end
  end
  if options.hotkeyStep ~= nil and options.hotkeyStep <= 0 then
    misuse(("%s: options.hotkeyStep must be above 0, got %s"):format(path, tostring(options.hotkeyStep)))
  end
  if options.labelPosition ~= nil and options.labelPosition ~= "left" and options.labelPosition ~= "above" and options.labelPosition ~= "right" then
    misuse(('%s: options.labelPosition must be "left", "above" or "right", got %q'):format(path, options.labelPosition))
  end
  if options.align ~= nil and not ROW_ALIGNS[options.align] then
    misuse(('%s: options.align must be one of "left", "center", "right", got %q'):format(path, options.align))
  end
  if options.columns ~= nil and keys.columns == "integer" and (options.columns < 1 or options.columns > 16) then
    misuse(("%s: options.columns must be 1 to 16, got %d"):format(path, options.columns))
  end
  if keys.alpha and options.value ~= nil then
    checkColor(path, "options.value", options.value)
  end
  return options
end

local function handlerBits(options)
  local bits = 0
  for name, bit in pairs(UI_HANDLER_BITS) do
    if options[name] ~= nil then
      bits = bits | bit
    end
  end
  return bits
end

local function props(options)
  local copy = {}
  for key, value in pairs(options) do
    if type(value) ~= "function" then
      copy[key] = value
    end
  end
  if options.image ~= nil then
    copy.image = imageIds[options.image]
    if not copy.image then
      misuse("options.image must be an image from manifold.ui.image.load")
    end
  end
  copy.handlers = handlerBits(options)
  return copy
end

local function keepHandlers(entry, options)
  for name in pairs(UI_HANDLER_BITS) do
    if options[name] ~= nil then
      entry[name] = options[name]
    end
  end
end

local function dropEntry(id)
  local entry = uiEntries[id]
  if not entry then
    return
  end
  uiEntries[id] = nil
  for child in pairs(entry.children) do
    dropEntry(child)
  end
  local parent = entry.parent and uiEntries[entry.parent]
  if parent then
    parent.children[id] = nil
  end
end

local widgetObject

local function addTo(path, parentEntry, kind, options)
  if not WIDGET_KEYS[kind] then
    misuse(('%s: argument 1 (kind) must be one of "%s", got %q'):format(path, table.concat(WIDGET_KINDS, '", "'), kind))
  end
  if parentEntry.kind ~= "window" and not CONTAINERS[parentEntry.kind] then
    misuse(("%s: a %s holds no widgets; add to a window, row, column, grid, section, tab or split"):format(path, parentEntry.kind))
  end
  if parentEntry.kind == "tabs" and kind ~= "tab" then
    misuse(path .. ": tabs hold tab widgets only")
  end
  if kind == "tab" and parentEntry.kind ~= "tabs" then
    misuse(path .. ": a tab goes into a tabs widget")
  end
  options = checkOptions(path .. "(" .. kind .. ")", WIDGET_KEYS[kind], options)
  if not native.uiExists(parentEntry.id) then
    return gone(parentEntry.kind)
  end
  local id, code, message = native.uiCreate(kind, parentEntry.id, parentEntry.owner, props(options))
  if not id then
    return result(nil, code, message)
  end
  local entry = { id = id, kind = kind, owner = parentEntry.owner, parent = parentEntry.id, children = {} }
  keepHandlers(entry, options)
  uiEntries[id] = entry
  parentEntry.children[id] = true
  entry.handle = widgetObject(entry)
  return entry.handle
end

local function liveEntry(entry)
  return uiEntries[entry.id] == entry and native.uiExists(entry.id)
end

local function updateEntry(path, entry, keys, options)
  options = checkOptions(path, keys, options)
  if not liveEntry(entry) then
    return gone(entry.kind)
  end
  keepHandlers(entry, options)
  local copy = props(options)
  local bits = 0
  for name, bit in pairs(UI_HANDLER_BITS) do
    if entry[name] ~= nil then
      bits = bits | bit
    end
  end
  copy.handlers = bits
  return result(native.uiUpdate(entry.id, copy))
end

local WIDGET_DOC = "manifold.ui.window.Widget."

widgetObject = function(entry)
  local prefix = ("widget(%s %d)"):format(entry.kind, entry.id)
  local proxy, members = strict(prefix)
  local function method(name, spec, body)
    bind(proxy, prefix .. "." .. name, members, name, spec, body, WIDGET_DOC .. name)
  end
  method("add", { params = { { name = "kind", type = "string" }, { name = "options", type = "table", optional = true } } }, function(kind, options)
    return addTo(prefix .. ".add", entry, kind, options)
  end)
  method("clear", {}, function()
    if not liveEntry(entry) then
      return gone(entry.kind)
    end
    for child in pairs(entry.children) do
      dropEntry(child)
    end
    entry.children = {}
    return result(native.uiClear(entry.id))
  end)
  method("remove", {}, function()
    if uiEntries[entry.id] ~= entry then
      return false
    end
    dropEntry(entry.id)
    return native.uiRemove(entry.id)
  end)
  method("update", { params = { { name = "options", type = "table" } } }, function(options)
    return updateEntry(prefix .. ".update", entry, WIDGET_KEYS[entry.kind], options)
  end)
  method("get", {}, function()
    if not liveEntry(entry) then
      return gone(entry.kind)
    end
    return native.uiGet(entry.id)
  end)
  method("set", { params = { { name = "value", type = "any", optional = true } } }, function(value)
    if not liveEntry(entry) then
      return gone(entry.kind)
    end
    return result(native.uiSet(entry.id, value))
  end)
  method("draw", { params = { { name = "drawings", type = "table" }, { name = "options", type = "table", optional = true, keys = { append = "boolean" } } } }, function(drawings, options)
    if entry.kind ~= "canvas" then
      misuse(prefix .. ".draw: only a canvas draws")
    end
    if not liveEntry(entry) then
      return gone("canvas")
    end
    local prepared = {}
    for index, drawing in ipairs(drawings) do
      if type(drawing) ~= "table" then
        misuse(("%s.draw: drawing %d must be a table, got %s"):format(prefix, index, kindOf(drawing)))
      end
      local allowed = DRAW_KEYS[drawing.kind]
      if not allowed then
        misuse(('%s.draw: drawing %d: kind must be one of "circle", "image", "line", "polygon", "rect", "text", got %s'):format(prefix, index, tostring(drawing.kind)))
      end
      for key in pairs(drawing) do
        if not allowed[key] then
          misuse(("%s.draw: drawing %d (%s) has no %s"):format(prefix, index, drawing.kind, tostring(key)))
        end
      end
      if drawing.color ~= nil then
        checkColor(prefix .. ".draw", "drawing " .. index .. " color", drawing.color)
      end
      if drawing.kind == "image" then
        local copy = {}
        for key, value in pairs(drawing) do
          copy[key] = value
        end
        copy.image = imageIds[drawing.image]
        if not copy.image then
          misuse(("%s.draw: drawing %d needs an image from manifold.ui.image.load"):format(prefix, index))
        end
        drawing = copy
      end
      prepared[index] = drawing
    end
    return result(native.uiDraw(entry.id, prepared, options and options.append))
  end)
  -- ∑ a table's rows and a combo's, radio's or tree's items, to read and to change one at a time
  local function rowsOf(name)
    if entry.kind ~= "table" then
      misuse(("%s.%s: only a table has rows"):format(prefix, name))
    end
    if not liveEntry(entry) then
      return nil
    end
    return (native.uiRows(entry.id))
  end
  local function rowIndex(name, rows, row, past)
    if math.type(row) ~= "integer" or row < 1 or row > #rows + (past and 1 or 0) then
      misuse(("%s.%s: row must be 1 to %d, got %s"):format(prefix, name, #rows + (past and 1 or 0), tostring(row)))
    end
  end
  local function cellsOf(name, cells)
    local copy = {}
    for index, cell in ipairs(cells) do
      if type(cell) ~= "string" and type(cell) ~= "number" and type(cell) ~= "boolean" then
        misuse(("%s.%s: cell %d must be a string, number or boolean, got %s"):format(prefix, name, index, kindOf(cell)))
      end
      copy[index] = tostring(cell)
    end
    return copy
  end
  method("getRows", {}, function()
    local rows = rowsOf("getRows")
    if not rows then
      return gone("table")
    end
    local _, columns = native.uiRows(entry.id)
    return rows, columns
  end)
  method("getRow", { params = { { name = "row", type = "integer", optional = true } } }, function(row)
    local rows = rowsOf("getRow")
    if not rows then
      return gone("table")
    end
    row = row or native.uiGet(entry.id)
    if row == nil then
      return nil
    end
    return rows[row]
  end)
  method("addRow", { params = { { name = "cells", type = "table" }, { name = "row", type = "integer", optional = true } } }, function(cells, row)
    local rows = rowsOf("addRow")
    if not rows then
      return gone("table")
    end
    row = row or #rows + 1
    rowIndex("addRow", rows, row, true)
    if #rows >= 100000 then
      -- full: the native side keeps the first 100000 rows (kMaxItems), one more would drop the last one unseen
      return false
    end
    table.insert(rows, row, cellsOf("addRow", cells))
    -- the selected row stays selected: a row inserted above it moves it down
    local selected = native.uiGet(entry.id)
    local update = { rows = rows }
    if selected ~= nil and row <= selected then
      update.value = selected + 1
    end
    return updateEntry(prefix .. ".addRow", entry, WIDGET_KEYS.table, update)
  end)
  method("setRow", { params = { { name = "row", type = "integer" }, { name = "cells", type = "table" } } }, function(row, cells)
    local rows = rowsOf("setRow")
    if not rows then
      return gone("table")
    end
    rowIndex("setRow", rows, row)
    rows[row] = cellsOf("setRow", cells)
    return updateEntry(prefix .. ".setRow", entry, WIDGET_KEYS.table, { rows = rows })
  end)
  method("removeRow", { params = { { name = "row", type = "integer" } } }, function(row)
    local rows = rowsOf("removeRow")
    if not rows then
      return gone("table")
    end
    rowIndex("removeRow", rows, row)
    table.remove(rows, row)
)MLUA"),
            std::string_view(R"MLUA(    -- the selection stays on its row: one above it moves it up, and removing the selected row clears it
    local selected = native.uiGet(entry.id)
    local update = { rows = rows }
    if selected ~= nil and row == selected then
      update.value = 0
    elseif selected ~= nil and row < selected then
      update.value = selected - 1
    end
    return updateEntry(prefix .. ".removeRow", entry, WIDGET_KEYS.table, update)
  end)
  method("getItems", {}, function()
    if entry.kind ~= "combo" and entry.kind ~= "radio" and entry.kind ~= "tree" then
      misuse(prefix .. ".getItems: only a combo, radio or tree has items")
    end
    if not liveEntry(entry) then
      return gone(entry.kind)
    end
    return native.uiItems(entry.id)
  end)
  method("getView", {}, function()
    if entry.kind ~= "canvas" then
      misuse(prefix .. ".getView: only a canvas has a view")
    end
    if not liveEntry(entry) then
      return gone("canvas")
    end
    return native.uiView(entry.id)
  end)
  method("setView", { params = { { name = "view", type = "table", keys = { x = "number", y = "number", scale = "number" } } } }, function(view)
    if entry.kind ~= "canvas" then
      misuse(prefix .. ".setView: only a canvas has a view")
    end
    if not liveEntry(entry) then
      return gone("canvas")
    end
    return result(native.uiSetView(entry.id, view))
  end)
  rawset(members, "id", entry.id)
  rawset(members, "kind", entry.kind)
  return proxy
end

local WINDOW_KEYS = { icon = "string", width = "number", height = "number", open = "boolean", dock = "boolean", interval = "number", key = "string", opacity = "number", inactiveOpacity = "number", labelPosition = "string",
  minWidth = "number", minHeight = "number", maxWidth = "number", maxHeight = "number", resizable = "boolean", autoSize = "boolean", scrollbar = "boolean", topmost = "boolean", modal = "boolean",
  position = "string", x = "number", y = "number", onClose = "function", onUpdate = "function" }
local WINDOW_UPDATE_KEYS = { title = "string", icon = "string", interval = "number", opacity = "number", inactiveOpacity = "number", labelPosition = "string",
  minWidth = "number", minHeight = "number", maxWidth = "number", maxHeight = "number", resizable = "boolean", autoSize = "boolean", scrollbar = "boolean", topmost = "boolean", modal = "boolean",
  onClose = "function", onUpdate = "function" }
local WINDOW_FLAG_KEYS = { "minWidth", "minHeight", "maxWidth", "maxHeight", "resizable", "autoSize", "scrollbar", "topmost", "modal" }
local WINDOW_POSITIONS = { center = true, mouse = true }
local WINDOW_DOC = "manifold.ui.window.Window."
local windowKeys = {}

local function windowObject(entry)
  local prefix = ("window(%q)"):format(entry.title)
  local proxy, members = strict(prefix)
  local function method(name, spec, body)
    bind(proxy, prefix .. "." .. name, members, name, spec, body, WINDOW_DOC .. name)
  end
  method("add", { params = { { name = "kind", type = "string" }, { name = "options", type = "table", optional = true } } }, function(kind, options)
    return addTo(prefix .. ".add", entry, kind, options)
  end)
  method("clear", {}, function()
    if not liveEntry(entry) then
      return gone("window")
    end
    for child in pairs(entry.children) do
      dropEntry(child)
    end
    entry.children = {}
    return result(native.uiClear(entry.id))
  end)
  method("remove", {}, function()
    if uiEntries[entry.id] ~= entry then
      return false
    end
    dropEntry(entry.id)
    forget(entry.owner, entry.id)
    if entry.key and windowKeys[entry.key] == entry then
      windowKeys[entry.key] = nil
    end
    return native.uiRemove(entry.id)
  end)
  method("update", { params = { { name = "options", type = "table" } } }, function(options)
    options = checkOptions(prefix .. ".update", WINDOW_UPDATE_KEYS, options)
    if not liveEntry(entry) then
      return gone("window")
    end
    if options.onClose ~= nil then
      entry.onClose = options.onClose
    end
    if options.onUpdate ~= nil then
      entry.onUpdate = options.onUpdate
    end
    local copy = { title = options.title, icon = options.icon, interval = options.interval, opacity = options.opacity, inactiveOpacity = options.inactiveOpacity, labelPosition = options.labelPosition, handlers = entry.onClose and UI_HANDLER_BITS.onClose or 0 }
    for _, key in ipairs(WINDOW_FLAG_KEYS) do
      copy[key] = options[key]
    end
    if options.onUpdate ~= nil then
      copy.updates = true
    end
    return result(native.uiUpdate(entry.id, copy))
  end)
  method("show", {}, function()
    if not liveEntry(entry) then
      return gone("window")
    end
    return result(native.uiShow(entry.id, true, false))
  end)
  method("hide", {}, function()
    if not liveEntry(entry) then
      return gone("window")
    end
    return result(native.uiShow(entry.id, false, false))
  end)
  method("focus", {}, function()
    if not liveEntry(entry) then
      return gone("window")
    end
    return result(native.uiShow(entry.id, true, true))
  end)
  method("isOpen", {}, function()
    return liveEntry(entry) and native.uiIsOpen(entry.id)
  end)
  rawset(members, "id", entry.id)
  rawset(members, "kind", "window")
  return proxy
end

local window = subarea(ui, "window", "Tool windows that dock into the workspace like Manifold's own. A window holds widgets; containers (`row`, `column`, `grid`, `section`, `tabs` with `tab`, `split`) hold more widgets.")

define(window, "create", {
  params = {
    { name = "title", type = "string" },
    { name = "options", type = "table", optional = true, keys = WINDOW_KEYS },
  },
  returns = "window",
  doc = "Opens a window and returns its handle. Options: `icon` (Font Awesome name), `width` and `height` (the first size, in points), `open` (false creates it hidden), `dock` (false keeps it out of the dock space), `opacity` (0.1 to 1, while the window has the focus) and `inactiveOpacity` (while another window has it; default `opacity`): without them the user's window opacity settings apply, and only a floating window can be see-through, a docked one stays opaque; `onClose(window)` when the user closes it (it is hidden, not removed; `show` brings it back), `onUpdate(window)` every `interval` seconds while it is open (default every frame), and `key`: a window created with the same key replaces the earlier one, and it keeps its place, size and dock spot from one run to the next. Size and behaviour: `minWidth`, `minHeight`, `maxWidth`, `maxHeight` (points) limit what the user can drag it to, `resizable = false` fixes the size, `autoSize = true` fits the window to its widgets, `scrollbar = false` hides the scroll bar, `topmost = true` keeps a floating window above other windows (it does not dock). `modal = true` shows the window as a dialog in front of the program: the rest of the program waits until it is closed (its close button, Escape or `hide`), a modal opened from a modal stacks on top of it, and closing one closes the ones opened from it; with `open = false` it waits for `show`. `position` places it the first time: `\"center\"` (of the main window) or `\"mouse\"`; `x` and `y` (points from the main window's corner) place it exactly.",
}, function(title, options)
  options = options or {}
  checkIcon("manifold.ui.window.create", options.icon)
  local owner = ownerSource()
  if options.key and windowKeys[options.key] then
    local earlier = windowKeys[options.key]
    if uiEntries[earlier.id] == earlier then
      earlier.handle.remove()
    end
  end
  if options.position ~= nil and not WINDOW_POSITIONS[options.position] then
    misuse(('manifold.ui.window.create: options.position must be "center" or "mouse", got %q'):format(options.position))
  end
  local props = {
    title = title,
    icon = options.icon,
    width = options.width,
    height = options.height,
    dock = options.dock,
    interval = options.interval,
    opacity = options.opacity,
    inactiveOpacity = options.inactiveOpacity,
    labelPosition = options.labelPosition,
    updates = options.onUpdate ~= nil,
    handlers = options.onClose and UI_HANDLER_BITS.onClose or 0,
    key = options.key,
    position = options.position,
    x = options.x,
    y = options.y,
  }
  for _, key in ipairs(WINDOW_FLAG_KEYS) do
    props[key] = options[key]
  end
  local id = native.uiCreate("window", 0, owner, props)
  if options.open == false then
    native.uiShow(id, false, false)
  end
  local entry = { id = id, kind = "window", owner = owner, children = {}, title = title, key = options.key, onClose = options.onClose, onUpdate = options.onUpdate }
  uiEntries[id] = entry
  entry.handle = windowObject(entry)
  if options.key then
    windowKeys[options.key] = entry
  end
  remember(owner, id, entry.handle.remove)
  return entry.handle
end)

native.docDefine(nil, "manifold.ui.window.Window", {
  kind = "class",
  title = "Window objects",
  doc = "`manifold.ui.window.create` returns a window object: dot calls only, read-only, unknown names raise. A call on a window that is gone (removed, or its script ran again) returns `nil, error` with code `ME-7010`.",
})
local WINDOW_METHOD_DOCS = {
  add = { params = { { name = "kind", type = "string" }, { name = "options", type = "table", optional = true } }, returns = "widget", doc = "Adds a widget at the end of the window and returns its handle. `kind` is one of `row`, `column`, `grid`, `section`, `tabs`, `split`, `text`, `heading`, `separator`, `spacer`, `button`, `checkbox`, `input`, `number`, `slider`, `combo`, `radio`, `color`, `table`, `tree`, `progress`, `image`, `canvas`; the options depend on it (see the widget object)." },
  clear = { returns = "boolean", doc = "Removes every widget of the window." },
  remove = { returns = "boolean", doc = "Removes the window and its widgets for good." },
  update = { params = { { name = "options", type = "table", keys = WINDOW_UPDATE_KEYS } }, returns = "boolean", doc = "Changes the title, icon, opacity (`opacity`, `inactiveOpacity`), size limits and behaviour (`minWidth`, `minHeight`, `maxWidth`, `maxHeight`, `resizable`, `autoSize`, `scrollbar`, `topmost`, `modal`), update interval or handlers." },
  show = { returns = "boolean", doc = "Shows the window again after the user or `hide` closed it." },
  hide = { returns = "boolean", doc = "Hides the window; `show` brings it back with its widgets." },
  focus = { returns = "boolean", doc = "Shows the window and brings it to the front." },
  isOpen = { returns = "boolean", doc = "Whether the window exists and is shown." },
}
for _, name in ipairs({ "add", "clear", "remove", "update", "show", "hide", "focus", "isOpen" }) do
  native.docDefine(nil, WINDOW_DOC .. name, WINDOW_METHOD_DOCS[name])
end
native.docDefine(nil, WINDOW_DOC .. "id", { kind = "field", type = "integer", doc = "The window's number." })
native.docDefine(nil, WINDOW_DOC .. "kind", { kind = "field", type = "string", doc = "Always `window`." })

native.docDefine(nil, "manifold.ui.window.Widget", {
  kind = "class",
  title = "Widget objects",
  doc = table.concat({
    "`add` returns a widget object, with the same rules as a window object. Every widget takes `visible`, `enabled`, `tooltip`, `width` and `height` (points; 0 or missing means automatic; a negative `width` fills the rest of the line); `minWidth` and `maxWidth` keep fields, buttons and lists within that range, and a text wraps at `maxWidth`. Nothing is drawn outside its place: a text, heading or button wider than its place ends in `...` and shows all of it as a tooltip; a text or heading with `overflow = \"wrap\"` breaks into lines instead (text only), `\"clip\"` is cut at the edge, `\"visible\"` keeps its own width. A window cannot be made narrower than its widgets need (their smallest sizes: a field 80 points, a button its padding and a few characters). Handlers get the widget as their last argument.",
)MLUA"),
            std::string_view(R"MLUA(    "- Labels of fields (`input`, `number`, `slider`, `combo`, `radio`, `color`): `labelPosition` is `left` (the default: the labels of one column, section or window share a column and the fields line up after it, and a checkbox among them lines up with the fields), `above` or `right` (after the field). Set on a window, column, section, tab or split it is the default for the fields inside. When none is set and a column is too narrow for the labels beside the fields, the labels go above them instead. A field fills the rest of its line unless it has a `width`, so no label is cut off; a negative `height` makes a table, tree, canvas, scrolling column or tab, multi-line input or section fill the height that is left.",
    "- `row`, `column`: lay out their widgets side by side or one below the other; a row's `align` (`left`, `center`, `right`) moves its widgets to that side of the line. In a row, fields, bars, cards, lists and columns without a `width` share the free width equally (a negative `width` asks for a share too), the others get what their content needs, every widget stays inside its share, a `spacer` without a width takes a share too (so what follows it goes to the right), and `wrap = true` moves a widget that does not fit to the next line instead of narrowing the widgets. `gap` (points) sets the space between the widgets of a row, column, section or tab; a column or tab with `scroll = true` scrolls inside its `height` (200).",
    "- `grid`: places its widgets in `columns` (2) cells per row, row by row. The first column fits its content and the others share the rest, so `text` then a field per row lines labels up; a field without a `label` fills its cell.",
    "- `section`: a card with `title`, `icon`, `badge`; `collapsible` lets the user fold it, `open = false` starts it folded.",
    "- `tabs` holds `tab` widgets (`title`, `icon`); `split` shows its first two widgets side by side (`vertical` stacks them) at `ratio` (0.5).",
    "- `text` (`text`, `color`, `wrap`, `mono`, `bold`, `size` in points, `align` = `left`, `center` or `right` for text that does not wrap), `heading` (`text`, `icon`, `color`, `size`, `align`), `separator` (`text` puts a caption into the line), `spacer`.",
    "- `button`: `text`, `icon` (an icon without text makes an icon button), `style` (`default`, `accent`, `danger`), `onClick(widget)`; `shortcut` (like `Ctrl+S` or `F5`) clicks it while its window has the focus, `confirm` asks that text before the click goes through; `hotkey` (like `F5` or `Ctrl+F2`) clicks it from anywhere, also while the game has the focus, as long as its window is open (not a button with `confirm`).",
    "- `checkbox` (`label`, boolean `value`; `style = \"switch\"` draws it as a switch; `hotkey` like `F1` or `Ctrl+F2` switches it from anywhere, also while the game has the focus, as long as its window is open and the box is enabled, and shows the key beside it), `input` (`label`, string `value`, `hint`, `multiline`, `password`, `readOnly`, `maxLength` in characters, `onSubmit(text, widget)` on Enter; `focus = true` puts the cursor there when the window opens, also on a `number`), `number` (`label`, `value`, `min`, `max`, `step`, `integer`, `hex`, `format` like `%.2f`; a typed value is reported when Enter is pressed or the field is left, a click on its +/- buttons at once), `slider` (`label`, `value`, `min`, `max`, `step`, `integer`, `format`; the value stays within `min` and `max`, also when typed with Ctrl+click; a `number` or `slider` takes `hotkeyIncrease` and `hotkeyDecrease`, keys that move it by `hotkeyStep` (else its `step`, else 1) from anywhere while its window is open, within `min` and `max`), `combo` (`label`, `items`, `hint` shown while nothing is chosen, `value` = the chosen item's number; with `multi = true` several items can be ticked and `value` is a list of their numbers, like `{ 1, 3 }`), `radio` (the same as `combo`, drawn as one button per item; `horizontal` puts them in one line; a `combo` or `radio` takes `hotkeyNext` and `hotkeyPrevious`, keys that choose the next or previous item, round the end), `color` (`label`, `value` = `#RRGGBB`, `alpha` for `#RRGGBBAA` with an alpha channel): all call `onChange(value, widget)`.",
    "- `table`: `columns` (titles, or `{ title, width, align }`), `rows` (lists of cells), `value` = the selected row's number, `onSelect(row, widget)`, `onActivate(row, widget)` on a double click, and `menu` (a list of entries; `-` is a separator) with `onMenu(choice, text, row, widget)`.",
    "- `tree`: `items` (`{ text, icon, id, open, children }`, or plain strings), `value` = the selected item's `id`, `onSelect(id, widget)`, `onActivate(id, widget)`, `menu` with `onMenu(choice, text, id, widget)`.",
    "- `progress` (`value` 0..1, `text`), `image` (`image` from `manifold.ui.image.load`).",
    "- `canvas`: draws what `draw` gave it on the GPU. `background` is a colour, `zoom = true` lets the user zoom with the wheel and pan by dragging; `onClick(x, y, button, widget)`, `onHover(x, y, widget)`, `onView(x, y, scale, widget)` and `menu` with `onMenu(choice, text, x, y, widget)` get canvas coordinates; `onKey(key, widget)` gets every key pressed (and repeated while held) while the canvas's window has the focus, by name: `LeftArrow`, `W`, `Space`, `Enter`, `F1`… (Escape also closes the window).",
  }, "\n"),
})
local WIDGET_METHOD_DOCS = {
  add = { params = { { name = "kind", type = "string" }, { name = "options", type = "table", optional = true } }, returns = "widget", doc = "Adds a widget to a container (`row`, `column`, `section`, `tab`, `split`; `tabs` take `tab` widgets only)." },
  clear = { returns = "boolean", doc = "Removes a container's widgets, or a canvas's drawings." },
  remove = { returns = "boolean", doc = "Removes the widget and everything in it." },
  update = { params = { { name = "options", type = "table", text = "Only the keys to change; the others keep their values." } }, returns = "boolean", doc = table.concat({
    "Changes options of the widget: the same keys `add` takes for its kind, handlers included, and nothing else (an unknown key raises and names the known ones). Keys that every widget takes: `visible`, `enabled` (false draws it dimmed and takes no input), `tooltip`, `width`, `height`, `minWidth`, `maxWidth`.",
    "By kind: a `text` or `heading` takes `text`; a `button` `text`, `icon`, `style`; a `checkbox` `label`, `value`, `style`; an `input` `label`, `value`, `hint`, `readOnly`; a `number` or `slider` `label`, `value`, `min`, `max`, `format`; a `combo` or `radio` `items` and `value` (new items keep the chosen number while it exists); a `progress` `value` (0..1) and `text`; a `table` `columns`, `rows`, `value`; a `tree` `items`, `value`; a `section` or `tab` `title`, `icon`, `badge`. A new `value` does not call `onChange`.",
    "Example: `w.speed.update({ value = 2, enabled = god.get() })`, `w.status.update({ text = \"Attached\", color = \"success\" })`.",
  }, " ") },
  get = { returns = "value", doc = "The widget's value: a checkbox's boolean, an input's text, a colour's `#RRGGBB`, a number, the chosen combo, radio or table row's number (a combo with `multi` returns the list of chosen numbers), the selected tree item's id, a progress fraction, otherwise the text. Returns nil when nothing is selected." },
  set = { params = { { name = "value", type = "any", optional = true } }, returns = "boolean", doc = "Sets the widget's value (see `get`); nil clears a selection. It does not call `onChange`." },
  draw = { params = { { name = "drawings", type = "table" }, { name = "options", type = "table", optional = true, keys = { append = "boolean" } } }, returns = "boolean", doc = "Replaces a canvas's drawings (`options.append` adds them instead). A drawing is a table with `kind`: `line` (`x`, `y`, `x2`, `y2`, `thickness`), `rect` (`x`, `y`, `width`, `height`, `filled`, `rounding`, `thickness`), `circle` (`x`, `y`, `radius`, `filled`, `thickness`), `polygon` (`points` = `{ x1, y1, x2, y2, ... }`, `filled`), `text` (`x`, `y`, `text`, `size`) or `image` (`image`, `x`, `y`, `width`, `height`); all but images take `color` (a theme name or `#RRGGBB`/`#RRGGBBAA`)." },
  getView = { returns = "table", doc = "A canvas's view `{ x, y, scale }`: the canvas point at its top left corner and the zoom." },
  setView = { params = { { name = "view", type = "table", keys = { x = "number", y = "number", scale = "number" } } }, returns = "boolean", doc = "Moves or zooms a canvas's view." },
  getRows = { returns = "table", doc = "A table's rows, each a list of its cells' texts, and as a second value the column titles. Example: `local rows, columns = w.list.getRows()`, `rows[2][1]` is the first cell of the second row." },
  getRow = { params = { { name = "row", type = "integer", optional = true, text = "The row's number; the selected row when left out." } }, returns = "table", doc = "One row of a table as a list of its cells' texts, or nil when there is no such row or nothing is selected. `w.list.getRow()` reads the selected row." },
  addRow = { params = { { name = "cells", type = "table", text = "The cells' texts; numbers and booleans are written as text." }, { name = "row", type = "integer", optional = true, text = "Where it goes; the end when left out." } }, returns = "boolean", doc = "Adds a row to a table, at the end or before `row`." },
  setRow = { params = { { name = "row", type = "integer" }, { name = "cells", type = "table" } }, returns = "boolean", doc = "Replaces a row of a table." },
  removeRow = { params = { { name = "row", type = "integer" } }, returns = "boolean", doc = "Removes a row of a table; the rows after it move up." },
  getItems = { returns = "table", doc = "A combo's or radio's items as a list of texts (`get` gives the chosen one's number), or a tree's items as tables with `id`, `text`, `icon`, `open` and `children`." },
}
for _, name in ipairs({ "add", "clear", "remove", "update", "get", "set", "draw", "getView", "setView", "getRows", "getRow", "addRow", "setRow", "removeRow", "getItems" }) do
  native.docDefine(nil, WIDGET_DOC .. name, WIDGET_METHOD_DOCS[name])
end
native.docDefine(nil, WIDGET_DOC .. "id", { kind = "field", type = "integer", doc = "The widget's number." })
native.docDefine(nil, WIDGET_DOC .. "kind", { kind = "field", type = "string", doc = "The kind it was added as." })

local form = subarea(ui, "form", "Windows described by a table, as the Form Editor (Tools > Form Editor) saves them in a `.form.lua` file. The table holds the layout and the names of the handlers; the script keeps the code and passes its handler functions in.")
-- ∑ Manifold.lua's main chunk is close to Lua's limit of 200 locals: new
-- form helpers live in tables or inside the functions that use them.
local FORM_SKIP = {
  file = { form = true, title = true, children = true, script = true, fragment = true, editor = true, modals = true },
  modal = { name = true, title = true, children = true },
  node = { kind = true, name = true, children = true, opens = true, closes = true, enabledWhen = true, record = true },
  -- the kinds a memory record can be bound to (record = "description" or id)
  bindable = { checkbox = true, button = true, number = true, slider = true, input = true, text = true, combo = true, radio = true },
}

-- ∑ enabledWhen: a widget of a form is enabled while another one says so,
-- without code. "god" (a checkbox or switch is on, a combo or radio has a
-- choice), "not god", or "mode = Multiplier, Double" (one of these items is
-- chosen). Returns the parsed rule, or nil and why not.
local function formWhen(text)
  if type(text) ~= "string" then
    return nil, "must be a string"
  end
  local negate, source, rest = text:match("^%s*(not%s+)([%a_][%w_]*)%s*(.-)%s*$")
  if not source then
    source, rest = text:match("^%s*([%a_][%w_]*)%s*(.-)%s*$")
  end
  if not source then
    return nil, 'must name a widget, like "god", "not god" or "mode = Multiplier"'
  end
)MLUA"),
            std::string_view(R"MLUA(  local rule = { source = source, negate = negate ~= nil }
  if rest ~= "" then
    local list = rest:match("^=%s*(.+)$")
    if not list or negate then
      return nil, 'must look like "god", "not god" or "mode = Multiplier, Double"'
    end
    rule.values = {}
    for item in list:gmatch("[^,]+") do
      rule.values[item:match("^%s*(.-)%s*$")] = true
    end
  end
  return rule
end
local formImageLoad
local callFormHandler

local FORM_VALUE_KINDS = { checkbox = true, input = true, number = true, slider = true, combo = true, radio = true, color = true, table = true, tree = true, progress = true }

local function formResolver(path, handlers)
  return function(where, key, value)
    if type(value) ~= "string" then
      misuse(("%s: %s.%s must be the name of a handler, got %s"):format(path, where, key, kindOf(value)))
    end
    if handlers == nil then
      return nil
    end
    local handler = handlers[value]
    if type(handler) ~= "function" then
      misuse(("%s: %s.%s names %q, but handlers.%s is %s"):format(path, where, key, value, value, handler == nil and "missing" or kindOf(handler)))
    end
    return handler
  end
end

local function formWindowOptions(path, spec, where, resolve, skip)
  local options = {}
  for key, value in pairs(spec) do
    if not skip[key] then
      if not WINDOW_KEYS[key] or key == "modal" then
        misuse(("%s: %s.%s is not a window option (known options: %s)"):format(path, where, tostring(key), table.concat(sortedNames(WINDOW_KEYS), ", ")))
      end
      if UI_HANDLER_BITS[key] then
        options[key] = resolve(where, key, value)
      else
        options[key] = value
      end
    end
  end
  if options.position ~= nil and not WINDOW_POSITIONS[options.position] then
    misuse(('%s: %s.position must be "center" or "mouse", got %s'):format(path, where, tostring(options.position)))
  end
  return options
end

local function formHeader(path, spec, handlers)
  if math.type(spec.form) ~= "integer" then
    misuse(path .. ": the table has no form = 1, so it is no form")
  end
  if spec.form > 1 then
    misuse(("%s: the form is from a newer Manifold (form = %d)"):format(path, spec.form))
  end
  if type(spec.title) ~= "string" then
    misuse(("%s: form.title must be a string, got %s"):format(path, kindOf(spec.title)))
  end
  local resolve = formResolver(path, handlers)
  return formWindowOptions(path, spec, "form", resolve, FORM_SKIP.file), resolve
end

local function formWidget(path, child, at, resolve)
  if type(child) ~= "table" then
    misuse(("%s: %s must be a table, got %s"):format(path, at, kindOf(child)))
  end
  if type(child.kind) ~= "string" then
    misuse(("%s: %s has no kind"):format(path, at))
  end
  if child.name ~= nil and (type(child.name) ~= "string" or not child.name:match("^[%a_][%w_]*$")) then
    misuse(("%s: %s.name must be a Lua name, got %s"):format(path, at, tostring(child.name)))
  end
  local label = child.name and ("%s %q"):format(child.kind, child.name) or at
  local keys = WIDGET_KEYS[child.kind]
  local copy = {}
  for key, value in pairs(child) do
    if not FORM_SKIP.node[key] then
      if keys and UI_HANDLER_BITS[key] and keys[key] then
        copy[key] = resolve(label, key, value)
      else
        copy[key] = value
      end
    end
  end
  return copy, label
end

-- ∑ The whole form is checked before a window is made or emptied, so a wrong
-- form raises without leaving half a window behind, and a live reload of a
-- wrong file keeps the window as it was.
local function checkForm(path, spec, handlers)
  local options, resolve = formHeader(path, spec, handlers)
  -- ∑ The modal forms of a form file: a list of window tables with a `name`.
  -- Each becomes a modal window that waits hidden for `show` (or a button's
  -- `opens`); `name` keys them in the third table that load and build return.
  local function formModals()
    local list = {}
    if spec.modals == nil then
      return list
    end
    if type(spec.modals) ~= "table" then
      misuse(("%s: form.modals must be a list of modal forms, got %s"):format(path, kindOf(spec.modals)))
    end
    local seen = {}
    for index, modal in ipairs(spec.modals) do
      local where = ("form.modals[%d]"):format(index)
      if type(modal) ~= "table" then
        misuse(("%s: %s must be a table, got %s"):format(path, where, kindOf(modal)))
      end
      if type(modal.name) ~= "string" or not modal.name:match("^[%a_][%w_]*$") then
        misuse(("%s: %s.name must be a Lua name, got %s"):format(path, where, tostring(modal.name)))
      end
      if seen[modal.name] then
        misuse(("%s: two modal forms are named %q"):format(path, modal.name))
      end
      seen[modal.name] = true
      if type(modal.title) ~= "string" then
        misuse(("%s: %s.title must be a string, got %s"):format(path, where, kindOf(modal.title)))
      end
      local options = formWindowOptions(path, modal, ("modal %q"):format(modal.name), resolve, FORM_SKIP.modal)
      options.modal = true
      options.open = false
      options.dock = nil
      list[#list + 1] = { name = modal.name, spec = modal, options = options, where = where }
    end
    return list
  end
  local modals = formModals()
  local modalNames = {}
  for _, modal in ipairs(modals) do
    modalNames[modal.name] = true
  end
  local names = {}
  local whens = {}
  local function walk(parentKind, list, where, depth)
    if list == nil then
      return
    end
    if type(list) ~= "table" then
      misuse(("%s: %s.children must be a list, got %s"):format(path, where, kindOf(list)))
    end
    if depth > 32 then
      misuse(path .. ": the form nests deeper than 32 levels")
    end
    for index, child in ipairs(list) do
      local copy, label = formWidget(path, child, ("%s.children[%d]"):format(where, index), resolve)
      local keys = WIDGET_KEYS[child.kind]
      if not keys then
        misuse(('%s: %s: kind must be one of "%s", got %q'):format(path, label, table.concat(WIDGET_KINDS, '", "'), child.kind))
      end
      if parentKind ~= "window" and not CONTAINERS[parentKind] then
        misuse(("%s: %s: a %s holds no widgets"):format(path, label, parentKind))
      end
      if parentKind == "tabs" and child.kind ~= "tab" then
        misuse(("%s: %s: tabs hold tab widgets only"):format(path, label))
      end
      if child.kind == "tab" and parentKind ~= "tabs" then
        misuse(("%s: %s: a tab goes into a tabs widget"):format(path, label))
      end
      if child.name then
        if names[child.name] then
          misuse(("%s: two widgets are named %q"):format(path, child.name))
        end
        names[child.name] = child
      end
      if child.enabledWhen ~= nil then
        local rule, problem = formWhen(child.enabledWhen)
        if not rule then
          misuse(("%s: %s.enabledWhen %s"):format(path, label, problem))
        end
        rule.label = label
        whens[#whens + 1] = rule
      end
      if child.record ~= nil then
        if (type(child.record) ~= "string" or child.record == "") and math.type(child.record) ~= "integer" then
          misuse(("%s: %s.record must be a memory record's description or id, got %s"):format(path, label, kindOf(child.record)))
        end
        if not FORM_SKIP.bindable[child.kind] then
          misuse(("%s: %s: a %s cannot show a memory record (a checkbox, button, number, slider, input, text, combo or radio can)"):format(path, label, child.kind))
        end
        if child.kind == "combo" and child.multi then
          misuse(("%s: %s: a combo with multi = true cannot show a memory record"):format(path, label))
        end
      end
      if child.opens ~= nil or child.closes ~= nil then
        if child.kind ~= "button" then
          misuse(("%s: %s: only a button opens or closes a window"):format(path, label))
        end
        if child.opens ~= nil and (type(child.opens) ~= "string" or not modalNames[child.opens]) then
          misuse(("%s: %s.opens must name one of the form's modals, got %s"):format(path, label, tostring(child.opens)))
        end
        if child.closes ~= nil and type(child.closes) ~= "boolean" then
          misuse(("%s: %s.closes must be a boolean, got %s"):format(path, label, kindOf(child.closes)))
        end
      end
      if child.kind == "image" and type(copy.image) == "string" then
        copy.image = nil
      end
      checkOptions(path .. "(" .. child.kind .. ")", keys, copy)
      walk(child.kind, child.children, label, depth + 1)
    end
  end
  walk("window", spec.children, "form", 1)
  for _, modal in ipairs(modals) do
    walk("window", modal.spec.children, modal.where, 1)
  end
  for _, rule in ipairs(whens) do
    local source = names[rule.source]
    if not source then
      misuse(("%s: %s.enabledWhen names %q, but no widget of the form has that name"):format(path, rule.label, rule.source))
    end
    if source.kind ~= "checkbox" and source.kind ~= "combo" and source.kind ~= "radio" then
      misuse(("%s: %s.enabledWhen names the %s %q; it must be a checkbox, combo or radio"):format(path, rule.label, source.kind, rule.source))
    end
    if rule.values and source.kind == "checkbox" then
      misuse(("%s: %s.enabledWhen: %q is a checkbox, which is on or off; leave out the \"= ...\""):format(path, rule.label, rule.source))
    end
  end
  return options, modals
end

-- `links.window` is the window the widgets go into (a button's `closes`
-- hides it), `links.modals` the form's modals by name (`opens` shows one).
local function fillForm(path, made, members, children, resolve, folder, links)
  -- `links.wiring` (shared by the form's windows) holds the enabledWhen rules
  -- by the name of the widget they listen to; `refresh(name)` applies them
  local wiring = links.wiring
  if wiring and not wiring.refresh then
    wiring.refresh = function(name)
      local list = wiring.rules[name]
      local source = rawget(members, name)
      if not list or not source then
        return
      end
      local ok, value = pcall(source.get)
      if not ok then
        return
      end
      -- the items the widget has now: the script may have changed them since the form was built
      local items = native.uiItems(source.id) or wiring.items[name] or {}
      local chosen = type(value) == "table" and value or { value }
      for _, rule in ipairs(list) do
        local on
        if rule.values then
          on = false
          for _, number in ipairs(chosen) do
            if rule.values[tostring(items[number])] or rule.values[tostring(number)] then
              on = true
            end
          end
        else
          on = value == true or (math.type(value) == "integer" and value > 0) or (type(value) == "table" and #value > 0)
        end
        if rule.negate then
          on = not on
        end
        if wiring.missing[rule.handle] then
          on = false
        end
        pcall(rule.handle.update, { enabled = on })
      end
    end

    -- ∑ record = "God Mode": the widget shows the memory record and changes
    -- it, without code. A checkbox follows whether the record is on, a
    -- button switches it, the other kinds show and write its value. The
    -- window refreshes them ten times a second while it is open; a widget
    -- whose record is not in the table is disabled until it is.
    wiring.binds = wiring.binds or {}
    wiring.missing = wiring.missing or {}
    -- a combo's or radio's items as they are now (the script may update them), else the form's
    local function itemsOf(bind)
      return bind.handle and native.uiItems(bind.handle.id) or bind.items or {}
    end
    local function shownValue(bind, value, text, entry)
      if bind.kind == "number" or bind.kind == "slider" then
        local number = type(value) == "number" and value or tonumber(text)
        -- an integer widget shows 1.5 as 2: compared unrounded, it never matched and was set again and again
)MLUA"),
            std::string_view(R"MLUA(        if bind.integer and math.type(number) == "float" and number == number and number > -2 ^ 63 and number < 2 ^ 63 then
          number = math.floor(number + 0.5)
        end
        return number
      end
      if bind.kind == "combo" or bind.kind == "radio" then
        -- an item names a drop-down entry by its description, or the value as shown
        for index, item in ipairs(itemsOf(bind)) do
          local named = tostring(item)
          if (entry ~= nil and named == entry) or named == text or named == tostring(value) then
            return index
          end
        end
        return nil
      end
      return text
    end
    -- a source of enabledWhen rules that a record just set: its rules follow
    local function followRules(bind)
      if bind.name and wiring.rules[bind.name] then
        wiring.refresh(bind.name)
      end
    end
    wiring.bindRefresh = function(windowHandle)
      for _, bind in ipairs(wiring.binds) do
        if bind.window == windowHandle then
          -- ∑ find takes a description only when exactly one record has it;
          -- with several, the widget stays disabled and the log says why once
          local record, missing = manifold.record.find(bind.record)
          local found = record ~= nil
          local problem = missing and missing.code or nil
          if problem ~= bind.problem then
            bind.problem = problem
            if problem == "ME-6011" then
              emit("warning", "manifold.ui", ("%s stays disabled: %s"):format(bind.label, missing.message))
            end
          end
          if found ~= bind.found then
            bind.found = found
            wiring.missing[bind.handle] = not found or nil
            if bind.rule then
              wiring.refresh(bind.rule)
            else
              pcall(bind.handle.update, { enabled = found and bind.enabled })
            end
            bind.last = nil
          end
          if record and bind.kind == "checkbox" then
            -- while its own switch is still being carried out, the record is not asked
            if not native.recordPending(record.id, native.recordIdentity()) then
              local active = record.isActive()
              if active ~= nil and active ~= bind.last then
                bind.last = active
                pcall(bind.handle.set, active)
                followRules(bind)
              end
            end
          elseif record and bind.kind ~= "button" then
            local value, text, entry = record.getValue()
            if value ~= nil then
              local shown = shownValue(bind, value, text, entry)
              local ok, current = pcall(bind.handle.get)
              -- a field the user is typing into keeps the keyboard: leave it until it is left
              local typing = (bind.kind == "input" or bind.kind == "number") and native.uiIsActive(bind.handle.id)
              if not typing and shown ~= nil and (shown ~= bind.last or not ok or current ~= shown) then
                bind.last = shown
                pcall(bind.handle.set, shown)
                followRules(bind)
              end
            end
          end
        end
      end
    end
    wiring.bindWrite = function(bind, value)
      local record, missing = manifold.record.find(bind.record)
      local ok, problem
      if not record then
        ok, problem = nil, missing
      elseif bind.kind == "checkbox" or bind.kind == "button" then
        -- like a click on the record's box: queued, so a long script does not freeze the program
        local wanted = value
        if bind.kind == "button" then
          wanted = not record.isActive()
        end
        ok, problem = native.recordRequestActive(record.id, native.recordIdentity(), wanted)
        if ok ~= nil and bind.kind == "checkbox" then
          bind.last = value
        end
      elseif bind.kind == "combo" or bind.kind == "radio" then
        local item = itemsOf(bind)[value]
        if item ~= nil then
          ok, problem = record.setValue(tostring(item))
          bind.last = value
        else
          ok = true
        end
      else
        ok, problem = record.setValue(value)
        -- what the field shows now is what was written: it follows the record again
        bind.last = value
      end
      if ok == nil then
        emit("warning", "manifold.ui", ("%s could not change the record %s: %s"):format(bind.label, tostring(bind.record), tostring(problem)))
        bind.last = nil
      end
    end
    -- after the form's windows are filled: each window with bound widgets refreshes them
    wiring.start = function()
      local windows = {}
      for _, bind in ipairs(wiring.binds) do
        windows[bind.window] = true
      end
      for windowHandle in pairs(windows) do
        local entry = uiEntries[windowHandle.id]
        if entry then
          entry.onBind = function()
            wiring.bindRefresh(windowHandle)
          end
          native.uiUpdate(windowHandle.id, { binds = true })
          wiring.bindRefresh(windowHandle)
        end
      end
    end
  end
  local function add(parent, list, where)
    for index, child in ipairs(list or {}) do
      local copy, label = formWidget(path, child, ("%s.children[%d]"):format(where, index), resolve)
      if child.kind == "image" and type(child.image) == "string" then
        local file = child.image
        if folder and not file:match("^%a:[\\/]") and not file:match("^[\\/]") then
          file = folder .. file
        end
        copy.image = formImageLoad(file)
        if copy.image and links.images then
          -- the form's pictures, freed when a reload replaces them
          links.images[#links.images + 1] = copy.image
        end
      end
      if child.kind == "button" and (child.opens or child.closes) then
        local original = copy.onClick
        local opens, closes = child.opens, child.closes
        copy.onClick = function(widget)
          if closes then
            links.window.hide()
          end
          if opens then
            local modal = rawget(links.modals, opens)
            if modal then
              modal.show()
            end
          end
          if original then
            return original(widget)
          end
        end
      end
      local bind
      if wiring and child.record ~= nil then
        bind = { record = child.record, kind = child.kind, items = child.items, integer = child.integer == true, window = links.window, label = label, name = child.name, enabled = child.enabled ~= false }
        if child.enabledWhen ~= nil then
          bind.rule = formWhen(child.enabledWhen).source
        end
        -- a text only shows the value: nothing to write back
        local handlerKey = child.kind ~= "text" and (child.kind == "button" and "onClick" or child.kind == "input" and "onSubmit" or "onChange") or nil
        local original = handlerKey and copy[handlerKey]
        if handlerKey then copy[handlerKey] = function(...)
          -- the record first, then the script's own handler with the same arguments
          local first = ...
          if child.kind == "button" then
            wiring.bindWrite(bind)
          else
            wiring.bindWrite(bind, first)
          end
          if original then
            return original(...)
          end
        end end
      end
      if wiring and child.name and (child.kind == "checkbox" or child.kind == "combo" or child.kind == "radio") then
        -- a widget others may listen to: its changes reach their rules first
        local original = copy.onChange
        local name = child.name
        wiring.items[name] = child.items
        copy.onChange = function(value, widget)
          wiring.refresh(name)
          if original then
            return original(value, widget)
          end
        end
      end
      local handle = parent.add(child.kind, copy)
      if child.name then
        rawset(members, child.name, handle)
      end
      if wiring and child.enabledWhen ~= nil then
        local rule = formWhen(child.enabledWhen)
        rule.handle = handle
        wiring.rules[rule.source] = wiring.rules[rule.source] or {}
        table.insert(wiring.rules[rule.source], rule)
      end
      if bind then
        bind.handle = handle
        wiring.binds[#wiring.binds + 1] = bind
      end
      add(handle, child.children, label)
    end
  end
  add(made, children, "form")
end

local function buildForm(path, spec, handlers, folder)
  local options, modals = checkForm(path, spec, handlers)
  local resolve = formResolver(path, handlers)
  local made = rawget(window.members, "create")(spec.title, options)
  local widgets, members = strict(("form(%q).widgets"):format(spec.title))
  local modalTable, modalMembers = strict(("form(%q).modals"):format(spec.title))
  local windows = { made }
  local wiring = { rules = {}, items = {} }
  local images = {}
  local ok, problem = pcall(function()
    for _, modal in ipairs(modals) do
      local handle = rawget(window.members, "create")(modal.spec.title, modal.options)
      windows[#windows + 1] = handle
      rawset(modalMembers, modal.name, handle)
    end
    fillForm(path, made, members, spec.children, resolve, folder, { window = made, modals = modalMembers, wiring = wiring, images = images })
    for _, modal in ipairs(modals) do
      local handle = rawget(modalMembers, modal.name)
      fillForm(path, handle, members, modal.spec.children, resolve, folder, { window = handle, modals = modalMembers, wiring = wiring, images = images })
    end
    for name in pairs(wiring.rules) do
      wiring.refresh(name)
    end
    if wiring.start then
      wiring.start()
    end
  end)
  if not ok then
    for _, made in ipairs(windows) do
      made.remove()
    end
    for _, picture in ipairs(images) do
      picture.remove()
    end
    error(problem, 0)
  end
  return made, widgets, members, modalTable, modalMembers, images
end

local function readFormFile(file, folder, block)
  local text, shown, message = native.uiFormRead(file, folder)
  if not text then
    return nil, shown, message
  end
  text = text:gsub("^\239\187\191", "") -- a UTF-8 BOM is no Lua
  if block then
    -- ∑ a script that holds its form: only the "local form = { ... }" table counts
    text = native.uiFormBlock(text)
    if not text then
      return nil, "ME-7014", shown .. " holds no form table (local form = { form = 1, ... })"
    end
  end
  local chunk, problem = load(text, "@" .. shown, "t", {})
  if not chunk then
    return nil, "ME-7014", problem
  end
  local ok, spec = pcall(chunk)
  if not ok or type(spec) ~= "table" then
    return nil, "ME-7014", ok and (shown .. " does not return a table") or tostring(spec)
  end
  return spec, shown
end

local function reloadForm(watch, start)
  if start then
    watch.entry = uiEntries[watch.window.id]
    watch.entry.onFormReload = function()
      reloadForm(watch)
    end
    native.uiWatch(watch.window.id, watch.file)
    return
  end
  if uiEntries[watch.window.id] ~= watch.entry then
    return
  end
  local function windowUpdate(title, options)
    -- ∑ an option the file no longer sets goes back to its default: only
    -- sending the keys that are there kept the old value until the next run
    local update = { title = title, icon = "", interval = 0, minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0,
      resizable = true, autoSize = false, scrollbar = true, topmost = false }
    for key in pairs(WINDOW_UPDATE_KEYS) do
      if key ~= "title" and options[key] ~= nil then
        update[key] = options[key]
      end
    end
    return update
  end
  -- ∑ update only sets the handlers it is given: one the file no longer
  -- names would keep running (onUpdate) or keep being called (onClose)
  local function windowHandlers(handle, options)
    local entry = uiEntries[handle.id]
    if entry then
      entry.onUpdate = options.onUpdate
      entry.onClose = options.onClose
)MLUA"),
            std::string_view(R"MLUA(      native.uiUpdate(handle.id, { updates = options.onUpdate ~= nil, handlers = options.onClose and UI_HANDLER_BITS.onClose or 0 })
    end
  end

  local spec, shown, message = readFormFile(watch.file, nil, watch.block)
  local options, modals
  if spec then
    local ok, checked, list = pcall(checkForm, "manifold.ui.form.load", spec, watch.handlers)
    if ok then
      options, modals = checked, list
    else
      message = isError(checked) and rawget(checked, "message") or tostring(checked)
    end
  else
    message = message or shown
  end
  if not options then
    emit("warning", "manifold.ui", ("%s changed, but the window keeps its layout: %s"):format(watch.file, tostring(message)))
    return
  end
  local values = {}
  for name, handle in pairs(watch.members) do
    if FORM_VALUE_KINDS[handle.kind] then
      local ok, value = pcall(handle.get)
      if ok and value ~= nil then
        values[name] = { kind = handle.kind, value = value }
      end
    end
  end
  local names = {}
  for name in pairs(watch.members) do
    names[#names + 1] = name
  end
  for _, name in ipairs(names) do
    rawset(watch.members, name, nil)
  end
  watch.window.clear()
  watch.window.update(windowUpdate(spec.title, options))
  windowHandlers(watch.window, options)
  -- modals: kept by name (cleared and refilled), new ones made, dropped ones removed
  local wanted = {}
  for _, modal in ipairs(modals) do
    wanted[modal.name] = modal
  end
  local existing = {}
  for name in pairs(watch.modals) do
    existing[#existing + 1] = name
  end
  for _, name in ipairs(existing) do
    if not wanted[name] then
      local handle = rawget(watch.modals, name)
      rawset(watch.modals, name, nil)
      handle.remove()
    end
  end
  local resolve = formResolver("manifold.ui.form.load", watch.handlers)
  local images = {}
  -- new modals and pictures belong to the form's script, as when it built the form
  ownerSource(true, watch.entry.owner)
  local ok, failure = pcall(function()
    for _, modal in ipairs(modals) do
      local handle = rawget(watch.modals, modal.name)
      if handle and uiEntries[handle.id] then
        handle.clear()
        local update = windowUpdate(modal.spec.title, modal.options)
        update.modal = true
        handle.update(update)
        windowHandlers(handle, modal.options)
      else
        handle = rawget(window.members, "create")(modal.spec.title, modal.options)
        rawset(watch.modals, modal.name, handle)
      end
    end
    local wiring = { rules = {}, items = {} }
    fillForm("manifold.ui.form.load", watch.window, watch.members, spec.children, resolve, watch.folder, { window = watch.window, modals = watch.modals, wiring = wiring, images = images })
    for _, modal in ipairs(modals) do
      local handle = rawget(watch.modals, modal.name)
      fillForm("manifold.ui.form.load", handle, watch.members, modal.spec.children, resolve, watch.folder, { window = handle, modals = watch.modals, wiring = wiring, images = images })
    end
    watch.wiring = wiring
  end)
  ownerSource(true, nil)
  -- the old widgets are gone, and with them the last use of the old pictures
  for _, picture in ipairs(watch.images or {}) do
    pcall(picture.remove)
  end
  watch.images = images
  for name, saved in pairs(values) do
    local handle = rawget(watch.members, name)
    if handle and handle.kind == saved.kind then
      pcall(handle.set, saved.value)
    end
  end
  if ok and watch.wiring and watch.wiring.refresh then
    for name in pairs(watch.wiring.rules) do
      watch.wiring.refresh(name)
    end
    -- the old bindings went with the old widgets
    local entry = uiEntries[watch.window.id]
    if entry then
      entry.onBind = nil
    end
    for _, handle in pairs(watch.modals) do
      entry = uiEntries[handle.id]
      if entry then
        entry.onBind = nil
      end
    end
    watch.wiring.start()
  end
  if not ok then
    emit("error", "manifold.ui", failure)
    return
  end
  emit("info", "manifold.ui", "Reloaded " .. watch.file)
  if watch.onReload then
    callFormHandler(watch.onReload, watch.window, watch.widgets, watch.modalTable)
  end
end

local FORM_RETURNS = "`window, widgets, modals`: the window object, a read-only table of the widgets that have a `name`, by name (the modals' widgets included; an unknown name raises), and a read-only table of the modal windows by name."

define(form, "build", {
  params = {
    { name = "form", type = "table" },
    { name = "handlers", type = "table", optional = true },
    { name = "options", type = "table", optional = true, keys = { reload = "boolean", onReload = "function" } },
  },
  returns = "window",
  doc = "Builds a window from a form table and returns " .. FORM_RETURNS .. " The table has `form = 1`, `title`, the window options (`icon`, `width`, `key`, `onClose`, ...) and `children`, a list of widget tables: `kind`, an optional `name`, the options of that kind and their own `children`. Handler options hold a name, like `onClick = \"save\"`, and are looked up in `handlers` (`handlers.save`); a missing one raises. Without `handlers` the handlers are left out, which shows what the form looks like." .. " `modals` lists modal forms: window tables like the form itself with a `name`, each made as a modal window (`modal = true`) that waits hidden; `modals.<name>.show()` opens it. A button's `opens = \"<name>\"` opens that modal when clicked and `closes = true` hides the window it is in; both run before the button's `onClick`." .. " Any widget can take `enabledWhen`, which enables it only while another widget of the form says so, without code: `\"god\"` (that checkbox or switch is on, or that combo or radio has a choice), `\"not god\"`, or `\"mode = Multiplier, Double\"` (one of these items of that combo or radio is chosen). It follows the user's changes; a script's `set` does not update it." .. " A checkbox, button, number, slider, input, text, combo or radio can take `record`: a memory record's description or id (see `manifold.record`), which binds the widget to it without code. A checkbox shows and switches whether the record is on, a button switches it, a number or slider shows and writes its value, an input writes when Enter is pressed, a text shows the value as the address list does, a combo or radio writes the chosen item's text (a drop-down entry of the record). The window refreshes them ten times a second while it is open, the script's own handlers still run after the write, and a widget whose record is not in the table is disabled until it is. A description binds only when exactly one record has it: with several, the widget stays disabled and a warning names them, until the descriptions differ or the form names the record by id." .. " The whole form is checked first, so a wrong one raises without making a window. An `image` widget takes a file name as `image`, relative to the calling script's folder; a picture that cannot be read leaves the widget empty." .. " A script file that holds its form as `local form = { form = 1, ... }` (as the Form Editor writes it) gets a live reload like `load`: saving the script rebuilds the window's widgets from the table in the file, with the same handlers; the handlers' own code changes only when the script runs again. `options.reload = false` switches that off, `options.onReload(window, widgets, modals)` runs after each rebuild.",
}, function(spec, handlers, options)
  options = options or {}
  local info = callerFrame()
  local source = info and info.source or ""
  local file = source:sub(1, 1) == "@" and source:sub(2) or nil
  local folder = file and file:match("^(.*[\\/])") or nil
  local made, widgets, members, modalTable, modalMembers, images = buildForm("manifold.ui.form.build", spec, handlers, folder)
  if file and options.reload ~= false then
    -- ∑ only when the file's form table is this form (a script may build others)
    local text = native.uiFormRead(file)
    local chunk = text and native.uiFormBlock(text)
    chunk = chunk and load(chunk, "=form", "t", {})
    local ok, found = pcall(chunk or error)
    if ok and type(found) == "table" and found.title == spec.title then
      reloadForm({ window = made, widgets = widgets, members = members, modals = modalMembers, modalTable = modalTable, file = file, block = true, handlers = handlers, folder = folder, onReload = options.onReload, images = images }, true)
    end
  end
  return made, widgets, modalTable
end)

define(form, "load", {
  params = {
    { name = "path", type = "string" },
    { name = "handlers", type = "table", optional = true },
    { name = "options", type = "table", optional = true, keys = { reload = "boolean", onReload = "function" } },
  },
  returns = "window",
  doc = "Reads a `.form.lua` file and builds its window like `build`; returns " .. FORM_RETURNS .. " A relative path starts at the folder of the calling script (at the program folder for code without a file). Returns nil and an `ME-7014` error when the file cannot be read or does not return a table; a wrong form raises like `build`. Picture files of `image` widgets are relative to the form file's folder. Live reload: while the window is open, saving the file (in the Form Editor or elsewhere) rebuilds its widgets in the same window, with the same handlers; the window, the `widgets` and `modals` tables stay the same objects (a modal keeps its window by name), widgets that keep their name and kind keep their value, and a wrong file only logs a warning. `options.reload = false` switches that off; `options.onReload(window, widgets, modals)` runs after each rebuild, to fill what the script put in (table rows, tree items, drawings).",
}, function(path, handlers, options)
  options = options or {}
  local info = callerFrame()
  local source = info and info.source or ""
  local folder = source:sub(1, 1) == "@" and source:sub(2):match("^(.*[\\/])") or nil
  local spec, shown, message = readFormFile(path, folder)
  if not spec then
    return result(nil, shown, message)
  end
  local formFolder = shown:match("^(.*[\\/])")
  local made, widgets, members, modalTable, modalMembers, images = buildForm("manifold.ui.form.load", spec, handlers, formFolder)
  if options.reload ~= false then
    reloadForm({ window = made, widgets = widgets, members = members, modals = modalMembers, modalTable = modalTable, file = shown, handlers = handlers, folder = formFolder, onReload = options.onReload, images = images }, true)
  end
  return made, widgets, modalTable
end)

local function plainObject(entry, prefix, docPrefix, methods, fields)
  local proxy, members = strict(prefix)
  for name, spec in pairs(methods) do
    bind(proxy, prefix .. "." .. name, members, name, spec, spec.body, docPrefix .. name)
  end
  rawset(members, "id", entry.id)
  for key, value in pairs(fields or {}) do
    rawset(members, key, value)
  end
  return proxy
end

local function dialogObject(entry)
  return plainObject(entry, ("dialog(%d)"):format(entry.id), "manifold.ui.dialog.Dialog.", {
    close = {
      body = function()
        if uiEntries[entry.id] ~= entry then
          return false
        end
        uiEntries[entry.id] = nil
        forget(entry.owner, entry.id)
        return native.uiDialogClose(entry.id)
      end,
    },
  })
end

native.docDefine(nil, "manifold.ui.dialog.Dialog", {
  kind = "class",
  title = "Dialog objects",
  doc = "The dialog functions return at once; the dialog shows on top of the program and calls `onClose` with the answer when the user is done.",
})
native.docDefine(nil, "manifold.ui.dialog.Dialog.close", { returns = "boolean", doc = "Closes the dialog without an answer; `onClose` is not called." })
native.docDefine(nil, "manifold.ui.dialog.Dialog.id", { kind = "field", type = "integer", doc = "The dialog's number." })

local dialog = subarea(ui, "dialog", "Dialogs on top of the program. They do not block the script: each takes `onClose`, which gets the answer, or nil when the user cancelled.")

)MLUA"),
            std::string_view(R"MLUA(local function openDialog(kind, props, onClose)
  local owner = ownerSource()
  local id = native.uiDialog(kind, owner, props)
  local entry = { id = id, kind = "dialog", owner = owner, children = {}, onDialog = onClose }
  uiEntries[id] = entry
  entry.handle = dialogObject(entry)
  remember(owner, id, entry.handle.close)
  return entry.handle
end

local DIALOG_KEYS = { title = "string", onClose = "function" }

define(dialog, "confirm", {
  params = {
    { name = "text", type = "string" },
    { name = "options", type = "table", optional = true, keys = { title = "string", danger = "boolean", onClose = "function" } },
  },
  returns = "dialog",
  doc = "Asks a yes/no question. `onClose(answer)` gets true or false; `danger = true` shows the yes button in the warning colour.",
}, function(text, options)
  options = options or {}
  return openDialog("confirm", { text = text, title = options.title, danger = options.danger }, options.onClose)
end)

define(dialog, "input", {
  params = {
    { name = "text", type = "string" },
    { name = "options", type = "table", optional = true, keys = { title = "string", value = "string", onClose = "function" } },
  },
  returns = "dialog",
  doc = "Asks for a line of text, starting from `value`. `onClose(text)` gets the text, or nil.",
}, function(text, options)
  options = options or {}
  return openDialog("input", { text = text, title = options.title, value = options.value }, options.onClose)
end)

define(dialog, "choose", {
  params = {
    { name = "text", type = "string" },
    { name = "items", type = "table" },
    { name = "options", type = "table", optional = true, keys = DIALOG_KEYS },
  },
  returns = "dialog",
  doc = "Lets the user pick one of `items`. `onClose(number, text)` gets the chosen item's number and text, or nil.",
}, function(text, items, options)
  options = options or {}
  local names = {}
  for index, item in ipairs(items) do
    names[index] = tostring(item)
  end
  return openDialog("choose", { text = text, title = options.title, items = names }, options.onClose)
end)

local FILE_KEYS = { title = "string", filters = "table", name = "string", onClose = "function" }
local FILE_TEXT = " `filters` is a list of `{ \"Lua scripts\", \"*.lua\" }` pairs; `onClose(path)` gets the full path, or nil."

local function flatFilters(path, filters)
  if filters == nil then
    return nil
  end
  local flat = {}
  for index, pair in ipairs(filters) do
    if type(pair) ~= "table" or type(pair[1]) ~= "string" or type(pair[2]) ~= "string" then
      misuse(("%s: filter %d must be { description, pattern }"):format(path, index))
    end
    flat[#flat + 1] = pair[1]
    flat[#flat + 1] = pair[2]
  end
  return flat
end

define(dialog, "openFile", {
  params = { { name = "options", type = "table", optional = true, keys = FILE_KEYS } },
  returns = "dialog",
  doc = "The Windows file dialog for opening a file." .. FILE_TEXT,
}, function(options)
  options = options or {}
  return openDialog("openFile", { title = options.title, value = options.name, filters = flatFilters("manifold.ui.dialog.openFile", options.filters) }, options.onClose)
end)

define(dialog, "saveFile", {
  params = { { name = "options", type = "table", optional = true, keys = FILE_KEYS } },
  returns = "dialog",
  doc = "The Windows file dialog for saving, starting with `name`." .. FILE_TEXT,
}, function(options)
  options = options or {}
  return openDialog("saveFile", { title = options.title, value = options.name, filters = flatFilters("manifold.ui.dialog.saveFile", options.filters) }, options.onClose)
end)

local NOTICE_LEVELS = { "info", "success", "warning", "error" }
local NOTICE_KEYS = { level = "string", progress = "number", sticky = "boolean" }

local notice = subarea(ui, "notice", "Short notices in the lower right corner, the same ones Manifold shows. A notice fades after a few seconds unless it is `sticky` or has a `progress`; a right click dismisses it.")

local function checkLevel(path, level)
  if level ~= nil then
    local known = false
    for _, name in ipairs(NOTICE_LEVELS) do
      known = known or name == level
    end
    if not known then
      misuse(('%s: options.level must be one of "info", "success", "warning", "error", got %q'):format(path, level))
    end
  end
end

local function noticeObject(entry)
  local state = entry.state
  return plainObject(entry, ("notice(%d)"):format(entry.id), "manifold.ui.notice.Notice.", {
    update = {
      params = { { name = "options", type = "table", keys = { text = "string", level = "string", progress = "number", sticky = "boolean" } } },
      body = function(options)
        checkLevel("notice.update", options.level)
        state.text = options.text or state.text
        state.level = options.level or state.level
        if options.progress ~= nil then
          state.progress = options.progress
        end
        if options.sticky ~= nil then
          state.sticky = options.sticky
        end
        local sticky = state.sticky or (state.progress >= 0 and state.progress < 1)
        if not native.uiNoticeUpdate(entry.id, state.text, state.level, state.progress, sticky) then
          return gone("notice")
        end
        return true
      end,
    },
    remove = {
      body = function()
        forget(entry.owner, "notice" .. entry.id)
        return native.uiNoticeRemove(entry.id)
      end,
    },
    isShown = {
      body = function()
        return native.uiNoticeExists(entry.id)
      end,
    },
  })
end

define(notice, "show", {
  params = {
    { name = "text", type = "string" },
    { name = "options", type = "table", optional = true, keys = NOTICE_KEYS },
  },
  returns = "notice",
  doc = "Shows a notice. `level` is `info` (default), `success`, `warning` or `error`; `progress` (0..1) adds a bar and keeps the notice up until it reaches 1; `sticky` keeps it until it is removed or dismissed.",
}, function(text, options)
  options = options or {}
  checkLevel("manifold.ui.notice.show", options.level)
  local progress = options.progress or -1
  local sticky = options.sticky or (progress >= 0 and progress < 1)
  local owner = ownerSource()
  local id = native.uiNotice(text, options.level or "info", progress, sticky)
  local entry = { id = id, owner = owner, state = { text = text, level = options.level or "info", progress = progress, sticky = options.sticky or false } }
  local handle = noticeObject(entry)
  remember(owner, "notice" .. id, handle.remove)
  return handle
end)

native.docDefine(nil, "manifold.ui.notice.Notice", { kind = "class", title = "Notice objects", doc = "`manifold.ui.notice.show` returns a notice object." })
native.docDefine(nil, "manifold.ui.notice.Notice.update", { params = { { name = "options", type = "table", keys = { text = "string", level = "string", progress = "number", sticky = "boolean" } } }, returns = "boolean", doc = "Changes the text, level, progress or stickiness, and shows the notice for its full time again." })
native.docDefine(nil, "manifold.ui.notice.Notice.remove", { returns = "boolean", doc = "Takes the notice down." })
native.docDefine(nil, "manifold.ui.notice.Notice.isShown", { returns = "boolean", doc = "Whether the notice is still up." })
native.docDefine(nil, "manifold.ui.notice.Notice.id", { kind = "field", type = "integer", doc = "The notice's number." })

local ENTRY_DOC = "manifold.ui.Entry."

local function entryObject(entry, keys)
  return plainObject(entry, ("%s(%d)"):format(entry.kind, entry.id), ENTRY_DOC, {
    update = {
      params = { { name = "options", type = "table" } },
      body = function(options)
        options = checkOptions(entry.kind .. ".update", keys, options)
        if uiEntries[entry.id] ~= entry then
          return gone("entry")
        end
        if options.onClick ~= nil then
          entry.onClick = options.onClick
        end
        local copy = {}
        for key, value in pairs(options) do
          if type(value) ~= "function" then
            copy[key] = value
          end
        end
        return result(native.uiSlotUpdate(entry.id, copy))
      end,
    },
    remove = {
      body = function()
        if uiEntries[entry.id] ~= entry then
          return false
        end
        uiEntries[entry.id] = nil
        forget(entry.owner, entry.id)
        return native.uiSlotRemove(entry.id)
      end,
    },
  })
end

local function addEntry(kind, keys, values, options)
  options = checkOptions("manifold.ui." .. kind .. ".add", keys, options)
  local owner = ownerSource()
  local copy = {}
  for key, value in pairs(values) do
    copy[key] = value
  end
  for key, value in pairs(options) do
    if type(value) ~= "function" then
      copy[key] = value
    end
  end
  local id = native.uiSlotAdd(kind, owner, copy)
  local entry = { id = id, kind = kind, owner = owner, children = {}, onClick = options.onClick }
  uiEntries[id] = entry
  entry.handle = entryObject(entry, keys)
  remember(owner, id, entry.handle.remove)
  return entry.handle
end

native.docDefine(nil, "manifold.ui.Entry", { kind = "class", title = "Menu, toolbar and status entries", doc = "`menu.add`, `toolbar.add` and `status.add` return an entry object; it takes the options its `add` took." })
native.docDefine(nil, ENTRY_DOC .. "update", { params = { { name = "options", type = "table" } }, returns = "boolean", doc = "Changes the entry: the options its `add` took." })
native.docDefine(nil, ENTRY_DOC .. "remove", { returns = "boolean", doc = "Takes the entry out." })
native.docDefine(nil, ENTRY_DOC .. "id", { kind = "field", type = "integer", doc = "The entry's number." })

local MENU_KEYS = { icon = "string", enabled = "boolean", checked = "boolean", onClick = "function" }
local menu = subarea(ui, "menu", "Entries in the main window's Extensions menu, beside the menus of Cheat Engine scripts. The menu shows as soon as a script adds one.")

define(menu, "add", {
  params = {
    { name = "path", type = "string" },
    { name = "options", type = "table", optional = true, keys = MENU_KEYS },
  },
  returns = "entry",
  doc = "Adds an entry. `path` may name submenus with slashes (`Teleporter/Save position`); `-` as the last part is a separator. Options: `icon`, `enabled`, `checked` (shows a check mark), `onClick(entry)`.",
}, function(path, options)
  local text = path:match("([^/]+)$") or path
  return addEntry("menu", MENU_KEYS, { path = path, text = text }, options)
end)

local TOOLBAR_KEYS = { text = "string", tooltip = "string", enabled = "boolean", onClick = "function", icon = "string" }
local toolbar = subarea(ui, "toolbar", "Buttons in the main window's title bar, between the process and the Memory view button.")

define(toolbar, "add", {
  params = {
    { name = "icon", type = "string" },
    { name = "options", type = "table", optional = true, keys = TOOLBAR_KEYS },
  },
  returns = "entry",
  doc = "Adds an icon button. Options: `text` beside the icon, `tooltip`, `enabled`, `onClick(entry)`.",
}, function(icon, options)
  checkIcon("manifold.ui.toolbar.add", icon)
  return addEntry("toolbar", TOOLBAR_KEYS, { icon = icon }, options)
end)

local STATUS_KEYS = { icon = "string", tooltip = "string", color = "string", onClick = "function", text = "string" }
local status = subarea(ui, "status", "Entries in the main window's status bar, after the administrator note.")

define(status, "add", {
  params = {
    { name = "text", type = "string" },
    { name = "options", type = "table", optional = true, keys = STATUS_KEYS },
  },
  returns = "entry",
  doc = "Adds a status entry. Options: `icon`, `tooltip`, `color` (a theme name), `onClick(entry)`.",
}, function(text, options)
  return addEntry("status", STATUS_KEYS, { text = text }, options)
end)

local image = subarea(ui, "image", "Pictures for `image` widgets and canvas drawings.")

define(image, "load", {
  params = { { name = "path", type = "string" } },
  returns = "image",
)MLUA"),
            std::string_view(R"MLUA(  doc = "Loads a PNG, JPEG, BMP, TGA or GIF file (a relative path starts at the program folder) and returns an image object with `width`, `height` and `remove()`, or nil and an `ME-7009` error.",
}, function(path)
  local owner = ownerSource()
  local id, width, height = native.uiImageLoad(path, owner)
  if not id then
    return result(nil, width, height)
  end
  local entry = { id = id, owner = owner }
  local handle = plainObject(entry, ("image(%d)"):format(id), "manifold.ui.image.Image.", {
    remove = {
      body = function()
        forget(owner, "image" .. id)
        return native.uiImageRemove(id)
      end,
    },
  }, { width = width, height = height })
  imageIds[handle] = id
  remember(owner, "image" .. id, handle.remove)
  return handle
end)

formImageLoad = function(file)
  return (rawget(image.members, "load")(file))
end

native.docDefine(nil, "manifold.ui.image.Image", { kind = "class", title = "Image objects", doc = "`manifold.ui.image.load` returns an image object." })
native.docDefine(nil, "manifold.ui.image.Image.remove", { returns = "boolean", doc = "Frees the picture; widgets and drawings that show it go blank." })
native.docDefine(nil, "manifold.ui.image.Image.id", { kind = "field", type = "integer", doc = "The image's number." })
native.docDefine(nil, "manifold.ui.image.Image.width", { kind = "field", type = "integer", doc = "Width in pixels." })
native.docDefine(nil, "manifold.ui.image.Image.height", { kind = "field", type = "integer", doc = "Height in pixels." })

define(ui, "getMode", {
  returns = "table",
  doc = "How the main window looks: `{ release = boolean, compact = boolean }`. *Release* shows three columns, hides the Edit, Tools and Debug menus and toggles records with a double click; *compact* hides the scanner and shows only the address list. View > Compact Mode and View > Release Mode (Ctrl+Shift+C, Ctrl+Shift+R) switch the same.",
}, function()
  return native.uiMode()
end)

define(ui, "setMode", {
  params = { { name = "mode", type = "table", keys = { release = "boolean", compact = "boolean" } } },
  returns = "nothing",
  doc = "Switches release and/or compact mode, like View > Release Mode and View > Compact Mode, and saves it with the settings. A key left out keeps its state, so `setMode{ compact = true }` changes only compact mode.",
}, function(mode)
  native.setUiMode(mode.release, mode.compact)
end)

local theme = subarea(ui, "theme", "The theme: the names `manifold.ui` takes for colours and icons, the look shown now, and custom themes. The presets are Standard, Red, Green and Blue; a custom theme is an .ini file in the themes folder that builds on one of them. `apply`, `override` and `restore` change the look for this session only, and it comes back when the script runs again or stops; `save` writes a theme file.")

define(theme, "colors", {
  returns = "table",
  doc = "The theme colour names, which follow the theme and light or dark mode.",
}, function()
  return table.move(UI_COLORS, 1, #UI_COLORS, 1, {})
end)

define(theme, "icons", {
  params = { { name = "filter", type = "string", optional = true } },
  returns = "table",
  doc = "The Font Awesome icon names, all of them or those that contain `filter`.",
}, function(filter)
  return native.uiIcons(filter)
end)

define(theme, "isIcon", {
  params = { { name = "name", type = "string" } },
  returns = "boolean",
  doc = "Whether `name` is a Font Awesome icon name.",
}, function(name)
  return native.uiIsIcon(name)
end)

-- ∑ in a do block: the main chunk is at Lua's 200 locals
do
  local THEME_MODES = { dark = true, light = true, system = true }

  define(theme, "sizes", {
    returns = "table",
    doc = "The size names a theme can set, such as `FrameRounding` or `ItemSpacing.x`, as Settings > Appearance lists them.",
  }, function()
    return native.uiThemeSizes()
  end)

  define(theme, "current", {
    returns = "table",
    doc = "The look shown now: `{ name, custom, base, mode, light, scripted }`. `name` is the preset's or the custom theme's name, `custom` whether it is a custom theme, `base` the preset it builds on (`standard`, `red`, `green` or `blue`), `mode` `dark`, `light` or `system`, `light` whether light colours show, `scripted` whether a script changed the look for this session.",
  }, function()
    return native.uiThemeCurrent()
  end)

  define(theme, "list", {
    returns = "table",
    doc = "The themes to pick from: the presets first, then the custom themes by name, each `{ name, custom, base }`.",
  }, function()
    return native.uiThemeList()
  end)

  define(theme, "color", {
    params = { { name = "name", type = "string" } },
    returns = "table, or nil, error",
    doc = "A colour of the look shown now as `{ r, g, b, a }` from 0 to 1. `name` is one of `colors()` or an ImGui colour as `imgui.WindowBg`; an unknown name gives `ThemeInvalid`.",
  }, function(name)
    return result(native.uiThemeColor(name))
  end)

  define(theme, "definition", {
    params = { { name = "name", type = "string" } },
    returns = "table, or nil, error",
    doc = "What a theme changes from its preset: `{ base, dark = { name = colour }, light = { ... }, sizes = { name = number } }`, to copy, change and `save`. A preset changes nothing, so its tables are empty; colours are `{ r, g, b, a }`. An unknown name gives `ThemeNotFound`.",
  }, function(name)
    return result(native.uiThemeDefinition(name))
  end)

  local function claimLook(owner)
    -- the look goes back when the script runs again or stops, or when its module unloads
    remember(owner, "theme", function() native.uiThemeRestore(owner) end)
    local module = callerModule()
    if module then
      own(module, function() native.uiThemeRestore(owner) end)
    end
  end

  define(theme, "apply", {
    params = {
      { name = "name", type = "string" },
      { name = "options", type = "table", optional = true, keys = { mode = "string" } },
    },
    returns = "true, or nil, error",
    doc = "Shows a preset or a custom theme for this session; the user's choice in the settings stays. `options.mode` is `dark`, `light` or `system` (default: the user's mode). An unknown name gives `ThemeNotFound`.",
  }, function(name, options)
    local mode = options and options.mode
    if mode ~= nil and not THEME_MODES[mode] then
      misuse("manifold.ui.theme.apply: options.mode must be one of \"dark\", \"light\", \"system\", got \"" .. tostring(mode) .. "\"")
    end
    local owner = ownerSource()
    local ok, code, message = native.uiThemeApply(name, mode, owner)
    if ok then
      claimLook(owner)
    end
    return result(ok, code, message)
  end)

  define(theme, "override", {
    params = { { name = "changes", type = "table", keys = { colors = "table", dark = "table", light = "table", sizes = "table" } } },
    returns = "true, or nil, error",
    doc = "Changes single colours or sizes of the look shown, for this session. `colors` applies to dark and light, `dark` and `light` to one mode; a colour is `\"#rrggbb\"`, `\"#rrggbbaa\"` or `{ r, g, b, a }` from 0 to 1. `sizes` takes the names of `sizes()`. A bad name or value gives `ThemeInvalid` and changes nothing.",
  }, function(changes)
    local owner = ownerSource()
    local ok, code, message = native.uiThemeOverride(changes, owner)
    if ok then
      claimLook(owner)
    end
    return result(ok, code, message)
  end)

  define(theme, "restore", {
    returns = "boolean",
    doc = "Undoes this script's `apply` and `override`, so the user's theme shows again; false when the look was not this script's.",
  }, function()
    return native.uiThemeRestore(ownerSource())
  end)

  define(theme, "save", {
    params = {
      { name = "name", type = "string" },
      { name = "definition", type = "table", keys = { base = "string", dark = "table", light = "table", sizes = "table" } },
      { name = "options", type = "table", optional = true, keys = { overwrite = "boolean" } },
    },
    returns = "true, or nil, error",
    doc = "Writes a custom theme file in the themes folder, as `definition()` describes one; `base` defaults to `standard`. It does not select the theme. A name that is taken gives `ThemeExists` unless `options.overwrite` replaces your own theme of that name; preset names are always taken.",
  }, function(name, definition, options)
    return result(native.uiThemeSave(name, definition, options and options.overwrite or false))
  end)
end

local function forgetSource(source)
  local list = uiOwned[source]
  if list then
    uiOwned[source] = nil
    for _, remove in pairs(list) do
      pcall(remove)
    end
  end
  native.uiForgetOwner(source)
  for key, entry in pairs(windowKeys) do
    if entry.owner == source then
      windowKeys[key] = nil
    end
  end
end

debug.getregistry()["Manifold.ChunkStarting"] = forgetSource

local moduleValues = {}
local modulePrivate = {}
local moduleUnloaders = {}
local moduleWarned = {}
local MODULE_LOADING = {}
local baseRequire = require

local function unloadModule(key)
  local handlers = moduleUnloaders[key]
  moduleUnloaders[key] = nil
  local handled = handlers ~= nil and #handlers > 0
  if handlers then
    for index = #handlers, 1, -1 do
      local ok, problem = xpcall(handlers[index], raisedHandler)
      if not ok then
        emit("error", "manifold.module", problem)
      end
    end
  end
  local owned = moduleOwned[key]
  moduleOwned[key] = nil
  if owned then
    for _, remove in pairs(owned) do
      pcall(remove)
    end
  end
  local sources = {}
  for source in pairs(uiOwned) do
    if native.moduleOfSource(source) == key then
      sources[#sources + 1] = source
    end
  end
  for _, source in ipairs(sources) do
    forgetSource(source)
  end
  moduleValues[key] = nil
  modulePrivate[key] = nil
  return handled
end

debug.getregistry()["Manifold.ModuleUnload"] = unloadModule

debug.getregistry()["Manifold.ModuleLoaded"] = function(key, value)
  if value == nil then
    value = true
  end
  moduleValues[key] = value
end

local function moduleRequire(name, ...)
  if type(name) ~= "string" then
    return baseRequire(name, ...)
  end
  local info = callerFrame()
  local key = info and native.moduleOfSource(info.source) or nil
  if key then
    local cache = modulePrivate[key]
    local cached = cache and cache[name]
    if cached == MODULE_LOADING then
      error("loop or previous error loading module '" .. name .. "'", 2)
    end
    if cached ~= nil then
      return cached
    end
    local path = native.moduleResolve(key, name)
    if path then
      local chunk, problem = native.moduleLoadFile(path)
      if not chunk then
        error("error loading module '" .. name .. "' from file '" .. path .. "':\n\t" .. tostring(problem), 2)
      end
      if not cache then
        cache = {}
        modulePrivate[key] = cache
      end
      cache[name] = MODULE_LOADING
      local ok, value = pcall(chunk, name, path)
      if not ok then
        cache[name] = nil
        error(value, 0)
      end
      if value == nil then
        value = true
      end
      if modulePrivate[key] == cache then
        cache[name] = value
      end
      return value, path
    end
  end
  if package.loaded[name] == nil then
    local other, loaded, declared, state = native.moduleLookup(name, key)
    if other then
      if not loaded then
        error("module '" .. name .. "' is an autorun module that is not loaded (" .. state .. ")", 2)
      end
      if key and not declared and not moduleWarned[key .. "\0" .. other] then
        moduleWarned[key .. "\0" .. other] = true
        emit("warning", "manifold.module", key .. " loads " .. name .. " with require, but does not name it in REQUIRES, so the load order is not guaranteed")
      end
      return moduleValues[other]
    end
  end
  return baseRequire(name, ...)
end

rawset(_G, "require", moduleRequire)
pcall(native.docBind, moduleRequire, "require")

)MLUA"),
            std::string_view(R"MLUA(local moduleArea = area("module", "The autorun modules: the scripts in the `autorun` folder and the modules in `autorun\\modules`. A module is named by the `NAME` line of its header, or by its file or folder name. Extensions > Modules shows the same list. [AUTORUN.md](AUTORUN.md) describes the folder layout, the header and what `require` finds.")

local MODULE_INFO = "a table with `name`, `version`, `author`, `description`, `category`, `kind` (`loose`, `file` or `folder`), `origin` (`shipped` or `user`), `state` (`loaded`, `disabled`, `failed`, `blocked`, `held`, `superseded`, `builtIn` or `pending`), `reason`, `enabled`, `changed`, `path` (the module's folder), `entry` (its entry file), `dataPath` (its folder under `autorun\\data`), `loadTime` in milliseconds and `requires` (a list of `{ name, version }`)"

define(moduleArea, "current", {
  returns = "table, or nil, error",
  doc = "The module the calling script belongs to, as " .. MODULE_INFO .. ". `ME-7011` when the caller is not an autorun module, such as a table script or the console.",
}, function()
  local key = callerModule()
  if not key then
    return result(nil, native.fail("LuaModuleUnknown", "(the calling script)"))
  end
  return result(native.moduleInfo(key))
end)

define(moduleArea, "ensureData", {
  returns = "string, or nil, error",
  doc = "Creates the calling module's data folder, `autorun\\data\\<name>\\`, when it is not there yet, and returns its path with a trailing backslash. A module keeps its settings and other files there: writing them does not count as a change to its code, and the module's own folder can be replaced as a whole. `ME-7011` when the caller is not an autorun module.",
}, function()
  local key = callerModule()
  if not key then
    return result(nil, native.fail("LuaModuleUnknown", "(the calling script)"))
  end
  return native.moduleEnsureData(key)
end)

define(moduleArea, "list", {
  returns = "table",
  doc = "Every script and module the autorun folder holds, in the order of the list in Extensions > Modules, each as the table `current` returns.",
}, function()
  return native.moduleList()
end)

define(moduleArea, "info", {
  params = { { name = "name", type = "string" } },
  returns = "table, or nil, error",
  doc = "One module by name, as the table `current` returns. `ME-7011` when there is none of that name.",
}, function(name)
  return result(native.moduleInfo(name))
end)

define(moduleArea, "reload", {
  params = { { name = "name", type = "string" } },
  returns = "true, or nil, error",
  doc = "Unloads a module and runs it again from disk, and after it the loaded modules that require it. The error is the reason the module did not load: `ME-7003` with the script error, `ME-7012` when it is blocked or disabled, `ME-7013` when it is held.",
}, function(name)
  return result(native.moduleReload(name))
end)

define(moduleArea, "enable", {
  params = { { name = "name", type = "string" } },
  returns = "true, or nil, error",
  doc = "Switches a module on, loads it now and remembers it for the next start. The error is the reason it did not load, as for `reload`.",
}, function(name)
  return result(native.moduleSetEnabled(name, true))
end)

define(moduleArea, "disable", {
  params = { { name = "name", type = "string" } },
  returns = "true, or nil, error",
  doc = "Switches a module off and remembers it for the next start. The module is unloaded, and so are the modules that require it. `info(name).enabled` tells which way the switch is.",
}, function(name)
  return result(native.moduleSetEnabled(name, false))
end)

define(moduleArea, "onUnload", {
  params = { { name = "handler", type = "function" } },
  returns = "true, or nil, error",
  doc = "Calls `handler()` when the calling module is unloaded: before it is reloaded, when it is switched off, and before all modules run again. The program removes what the module registered through `manifold` on its own; the handler takes down the rest, such as Cheat Engine menu items, forms and timers. Handlers run in reverse order. `ME-7011` when the caller is not an autorun module.",
}, function(handler)
  local key = callerModule()
  if not key then
    return result(nil, native.fail("LuaModuleUnknown", "(the calling script)"))
  end
  local handlers = moduleUnloaders[key]
  if not handlers then
    handlers = {}
    moduleUnloaders[key] = handlers
  end
  handlers[#handlers + 1] = handler
  return true
end)

local function callHandler(handler, ...)
  local ok, problem = xpcall(handler, raisedHandler, ...)
  if not ok then
    emit("error", "manifold.ui", problem)
  end
  return ok
end

callFormHandler = callHandler

debug.getregistry()["Manifold.UiTick"] = function(now)
  for _, id in ipairs(native.uiDue(now)) do
    local entry = uiEntries[id]
    if entry and entry.onUpdate and not callHandler(entry.onUpdate, entry.handle) then
      native.uiUpdate(id, { updates = false })
    end
  end
  for _, event in ipairs(native.uiTakeEvents()) do
    local entry = uiEntries[event.target]
    if entry then
      if event.name == "dialog" then
        uiEntries[event.target] = nil
        forget(entry.owner, entry.id)
        if entry.onDialog then
          callHandler(entry.onDialog, table.unpack(event, 1, event.n))
        end
      else
        local handler = entry[UI_EVENTS[event.name] or ""]
        if handler then
          local values = table.move(event, 1, event.n, 1, {})
          if event.name == "change" and entry.kind == "combo" and type(values[1]) == "string" then
            -- a combo with multi = true sends its chosen item numbers as "1,3"
            local list = {}
            for number in values[1]:gmatch("%d+") do
              list[#list + 1] = tonumber(number)
            end
            values[1] = list
          end
          values[event.n + 1] = entry.handle
          callHandler(handler, table.unpack(values, 1, event.n + 1))
        end
      end
    end
  end
  -- widgets bound to memory records (record = ... in a form), after the
  -- events: what the user just typed is written before the record is read
  for _, id in ipairs(native.uiBindsDue(now)) do
    local entry = uiEntries[id]
    if entry and entry.onBind and not callHandler(entry.onBind) then
      entry.onBind = nil
    end
  end
end

rawset(_G, "manifold", manifold)

local INTRO = [[
# Manifold Lua API

`manifold` is Manifold Engine's own Lua API. It lives beside the Cheat Engine functions in [LUA-API.md](LUA-API.md). Those keep Cheat Engine's names and habits for compatibility; `manifold` follows one fixed schema instead, and every call is checked against it.

This file is generated from the API itself (`Manifold-Engine.Tests.exe --manifold-markdown`), and the tests fail when it is out of date.
]]

local RULES = [[
## Schema

1. **One namespace.** Everything lives in the global table `manifold`. Its fields are areas (`manifold.log`, `manifold.process`, …), constants (`manifold.apiVersion`, `manifold.version`) and `manifold.has`. An area may group its functions into sub-areas one level down (`manifold.ui.window`, `manifold.ui.dialog`).
2. **Names.** Areas are lower-case nouns. Functions are lowerCamelCase: a lookup without side effects is named after what it returns (`process.module`, `log.channel`), everything else starts with a verb (`open`, `read`, `create`). Predicates start with `is` or `has` and return a boolean. Settings come in `get…`/`set…` pairs. The five log functions are named after their level. Constants are lowerCamelCase fields. There are no aliases and no capitalised duplicates.
3. **Calls use a dot.** `manifold.log.info("ready")`, never `manifold.log:info("ready")`. No function takes `self`, and the objects the API returns (channels) work the same way. A colon call raises an error that says so.
4. **Arguments.** Required arguments come first, optional ones after them, and settings go into one `options` table at the end. A `?` after a name marks it optional. A missing, extra or wrongly typed argument and an unknown option key raise `ME-7007`, naming the function and the argument. Addresses are integers or address expressions such as `"game.exe+10"`.
5. **Results.** A function that can fail while the script runs (no process, unreadable memory, unknown symbol) returns its value on success, or `true` when it has none, and `nil, error` on failure. It does not raise for those. Predicates never fail.
6. **Errors.** An error is a table `{ code, message, where, traceback }`: `code` is the `ME-` code from [ERRORS.md](ERRORS.md) or a script's own code, `message` the text, `where` the script line that made the call (`chunk:line`), `traceback` the stack when `manifold.error.try` caught it. `tostring(error)` gives `where: message (code)`. Using the API the wrong way (rules 3 and 4, unknown names) raises such an error instead of returning it.
7. **Strict tables.** Reading a name that does not exist raises `ME-7007` with the nearest name (`manifold.log.warn` → `manifold.log.warning`), and every table is read-only. `manifold.has("log.channel")` tests for a name without raising.
8. **Versions.** `manifold.apiVersion` grows when functions are added. Existing names keep their meaning.

```lua
local log = manifold.log.channel("Teleporter")
local game, problem = manifold.process.info()
if not game then
  log.warning("no game open", { reason = problem.message })
  return
end
local ok, failure = manifold.error.try(function()
  local base = manifold.error.check(manifold.process.address("game.exe+1A2B30"))
  local health = manifold.error.check(manifold.memory.read(base + 0x10, "i32"))
  log.info("player found", { base = base, health = health })
end)
if not ok then
  log.error(failure)
end
```
]]

local function markdown()
  return table.concat({ INTRO, RULES, native.docMarkdown("manifold") }, "\n")
end

return markdown
)MLUA"),
        };
        constexpr std::array<std::string_view, 1> kManifold1 = {
            std::string_view(R"MLUA(local manifold = manifold
local rtti = manifold.rtti

local RTTI = {}
RTTI.VERSION = "3.0.0"

local function ready()
  if not manifold.process.isOpen() then
    error("no process is opened", 3)
  end
end

local function toAddress(value)
  if math.type(value) == "integer" then
    return value
  end
  if type(value) == "number" then
    return math.tointeger(math.floor(value))
  end
  if type(value) == "string" and value ~= "" then
    local address = manifold.process.address(value)
    if address and address ~= 0 then
      return address
    end
  end
  return nil
end

function RTTI.demangle(decorated)
  if type(decorated) ~= "string" then
    return nil
  end
  return rtti.demangle(decorated)
end

function RTTI.mangle(name)
  if type(name) ~= "string" then
    return nil
  end
  return rtti.mangle(name)
end

function RTTI.className(address)
  ready()
  local at = toAddress(address)
  return at and (rtti.className(at)) or nil
end

function RTTI.object(address)
  ready()
  local at = toAddress(address)
  return at and (rtti.object(at)) or nil
end

function RTTI.owner(address, window)
  ready()
  local at = toAddress(address)
  if not at then
    return nil
  end
  return (rtti.owner(at, math.type(window) == "integer" and window or nil))
end

function RTTI.whois(address)
  ready()
  local at = toAddress(address)
  return at and (rtti.whois(at)) or nil
end

function RTTI.bases(what)
  ready()
  if type(what) ~= "string" then
    what = toAddress(what)
  end
  return what and (rtti.bases(what)) or nil
end

function RTTI.vtable(name, base)
  ready()
  if type(name) ~= "string" then
    return nil
  end
  return (rtti.vtable(name, type(base) == "string" and base or nil))
end

function RTTI.vtables(name)
  ready()
  if type(name) ~= "string" then
    return nil
  end
  return (rtti.vtables(name))
end

function RTTI.instances(name, limit)
  ready()
  if type(name) ~= "string" then
    return nil
  end
  local count = type(limit) == "number" and math.tointeger(math.floor(limit)) or nil
  return (rtti.instances(name, count))
end

function RTTI.dissect(address)
  ready()
  local at = toAddress(address)
  local ok = at and rtti.dissect(at)
  if not ok then
    error(string.format("no object with run-time type information at %X", at or 0), 2)
  end
  return true
end

function RTTI.label(structure, address)
  ready()
  local at = toAddress(address)
  return at and (rtti.label(structure, at)) or 0
end

function RTTI.classes()
  ready()
  return (rtti.classes())
end

function RTTI.status()
  return rtti.status().text
end

function RTTI.reset()
  rtti.reset()
end

rawset(_G, "ManifoldRTTI", RTTI)

manifold.doc.registerModule("ManifoldRTTI", RTTI, { doc = "The names of the Manifold-RTTI autorun script, kept for tables and scripts written against it. The run-time type information is built into the program now: these functions call manifold.rtti and return what the script returned (nil where it found nothing, an error without a process)." })
)MLUA"),
        };
        constexpr std::array<std::string_view, 1> kManifold2 = {
            std::string_view(R"MLUA(local manifold = manifold
local signature = manifold.signature

local SigMaker = {}
SigMaker.VERSION = "3.0.0"

local function text(problem, fallback)
  if type(problem) == "table" and problem.message then
    return problem.message
  end
  return problem ~= nil and tostring(problem) or fallback
end

local function selected()
  local ok, address = pcall(function() return getMemoryViewForm().DisassemblerView.SelectedAddress end)
  if ok and math.type(address) == "integer" and address ~= 0 then
    return address
  end
  return nil
end

function SigMaker:Make(address)
  address = address or selected()
  if address == nil then
    return nil, "no address was given"
  end
  local made, problem = signature.make(address)
  if not made then
    return nil, text(problem, "no signature")
  end
  return made
end

function SigMaker:Pattern(address)
  local made, problem = self:Make(address)
  if not made then
    return nil, problem
  end
  return made.pattern, made
end

function SigMaker:Copy(address)
  local made, problem = self:Make(address)
  if not made then
    return nil, problem
  end
  if type(writeToClipboard) == "function" then
    writeToClipboard(made.pattern)
  end
  return made.pattern
end

function SigMaker:Scan(pattern)
  local hits, problem = signature.find(pattern)
  if not hits then
    return nil, text(problem, "not a signature")
  end
  return hits
end

function SigMaker:Find(pattern)
  return self:Scan(pattern)
end

function SigMaker:Status()
  return "The signature maker is built into Manifold Engine " .. tostring(manifold.version) .. " (manifold.signature)."
end

rawset(_G, "ManifoldSigMaker", SigMaker)

manifold.doc.registerModule("ManifoldSigMaker", SigMaker, { doc = "The names of the Manifold-SigMaker autorun module, kept for tables and scripts written against it. The signature maker is built into the program now: Make, Pattern, Copy and Scan call manifold.signature with the settings under Settings > Signatures. Call them with a colon." })
)MLUA"),
        };
        constexpr std::array<std::string_view, 2> kDoc0 = {
            std::string_view(R"MLUA(---@meta

---Writes the values to the Lua output, separated by tabs.
---@param ... any
function print(...) end

---Iterates over all keys and values of a table, in no particular order.
---@param table table
---@return function, table, nil
function pairs(table) end

---Iterates over the array part of a table, from 1 up to the first nil.
---@param table table
---@return function, table, integer
function ipairs(table) end

---Returns the next key and value of a table, or nil after the last one.
---@param table table
---@param key? any
---@return any, any
function next(table, key) end

---Returns the type name of the value.
---@param value any
---@return "nil"|"number"|"string"|"boolean"|"table"|"function"|"thread"|"userdata"
function type(value) end

---Converts the value to a string, through `__tostring` when it has one.
---@param value any
---@return string
function tostring(value) end

---Converts the value to a number, or nil when it is not one.
---@param value any
---@param base? integer # 2 to 36; the value must then be a string
---@return number|nil
function tonumber(value, base) end

---Returns the arguments after index, or their count for "#".
---@param index integer|"#"
---@param ... any
---@return any
function select(index, ...) end

---Calls the function in protected mode and returns whether it succeeded, then its results or the error.
---@param f function
---@param ... any
---@return boolean, any
function pcall(f, ...) end

---Like pcall, with a message handler that gets the error first.
---@param f function
---@param handler function
---@param ... any
---@return boolean, any
function xpcall(f, handler, ...) end

---Raises an error.
---@param message any
---@param level? integer # 1 (the default) blames the caller, 2 its caller, 0 adds no position
function error(message, level) end

---Raises an error when the value is false or nil, and returns all arguments otherwise.
---@param value any
---@param message? any
---@return any
function assert(value, message) end

---Sets the metatable of a table and returns the table.
---@param table table
---@param metatable table|nil
---@return table
function setmetatable(table, metatable) end

---Returns the metatable of a value, or its `__metatable` field.
---@param value any
---@return table|nil
function getmetatable(value) end

---Reads a field without metamethods.
---@param table table
---@param key any
---@return any
function rawget(table, key) end

---Writes a field without metamethods.
---@param table table
---@param key any
---@param value any
---@return table
function rawset(table, key, value) end

---Compares two values without metamethods.
---@param a any
---@param b any
---@return boolean
function rawequal(a, b) end

---The length of a table or string without metamethods.
---@param value table|string
---@return integer
function rawlen(value) end

---Loads a module and returns what it returned.
---@param name string
---@return any
function require(name) end

---Compiles a chunk and returns it as a function, or nil and the error.
---@param chunk string|function
---@param name? string
---@param mode? "b"|"t"|"bt"
---@param env? table
---@return function|nil, string|nil
function load(chunk, name, mode, env) end

---Compiles a file and returns it as a function, or nil and the error.
---@param filename? string
---@param mode? "b"|"t"|"bt"
---@param env? table
---@return function|nil, string|nil
function loadfile(filename, mode, env) end

---Runs a file and returns its results.
---@param filename? string
---@return any
function dofile(filename) end

---Controls the garbage collector.
---@param option? "collect"|"stop"|"restart"|"count"|"step"|"isrunning"|"incremental"|"generational"
---@param ... any
---@return any
function collectgarbage(option, ...) end

---Formats the values like C's printf: %d %i %u %c %x %X %o %e %E %f %g %G %q %s %a %A %%.
---@param format string
---@param ... any
---@return string
function string.format(format, ...) end

---Returns the part from i to j; negative positions count from the end.
---@param s string
---@param i integer
---@param j? integer
---@return string
function string.sub(s, i, j) end

---Finds the first match of a pattern and returns its start, end and captures.
---@param s string
---@param pattern string
---@param init? integer
---@param plain? boolean # true searches for the text itself, without pattern characters
---@return integer|nil, integer|nil, any
function string.find(s, pattern, init, plain) end

---Returns the captures of the first match, or the whole match.
---@param s string
---@param pattern string
---@param init? integer
---@return any
function string.match(s, pattern, init) end

---Iterates over all matches of a pattern.
---@param s string
---@param pattern string
---@param init? integer
---@return function
function string.gmatch(s, pattern, init) end

---Replaces the matches of a pattern and returns the text and the number of replacements.
---@param s string
---@param pattern string
---@param replacement string|table|function
---@param n? integer
---@return string, integer
function string.gsub(s, pattern, replacement, n) end

---Repeats a string n times.
---@param s string
---@param n integer
---@param separator? string
---@return string
function string.rep(s, n, separator) end

---Returns the character codes of s from i to j.
---@param s string
---@param i? integer
---@param j? integer
---@return integer
function string.byte(s, i, j) end

---Builds a string from character codes.
---@param ... integer
---@return string
function string.char(...) end

---Converts to upper case.
---@param s string
---@return string
function string.upper(s) end

---Converts to lower case.
---@param s string
---@return string
function string.lower(s) end

---Returns the length in bytes.
---@param s string
---@return integer
function string.len(s) end

---Reverses a string.
---@param s string
---@return string
function string.reverse(s) end

---Packs the values into a binary string, such as "<I4" for a little-endian 32-bit integer.
---@param format string
---@param ... any
---@return string
function string.pack(format, ...) end

---Unpacks values from a binary string; the last result is the position after them.
---@param format string
---@param s string
---@param position? integer
---@return any
function string.unpack(format, s, position) end

---The size of a packed format in bytes.
---@param format string
---@return integer
function string.packsize(format) end

---Inserts a value at the end, or at a position.
---@param table table
---@param position? integer
---@param value any
function table.insert(table, position, value) end

---Removes the value at a position, or the last one, and returns it.
---@param table table
---@param position? integer
---@return any
function table.remove(table, position) end

---Joins the values from i to j into a string.
---@param table table
---@param separator? string
---@param i? integer
---@param j? integer
---@return string
function table.concat(table, separator, i, j) end

---Sorts the table in place.
---@param table table
---@param compare? fun(a: any, b: any): boolean
function table.sort(table, compare) end

---Returns the values from i to j as separate results.
---@param table table
---@param i? integer
---@param j? integer
---@return any
function table.unpack(table, i, j) end

---Returns the arguments as a table with the field n.
---@param ... any
---@return table
function table.pack(...) end

---Moves the elements f to e of a1 to position t of a2 (or a1) and returns a2.
---@param a1 table
---@param f integer
---@param e integer
---@param t integer
---@param a2? table
---@return table
function table.move(a1, f, e, t, a2) end

---The largest integer not above x.
---@param x number
---@return integer
function math.floor(x) end

---The smallest integer not below x.
---@param x number
---@return integer
function math.ceil(x) end

---The absolute value.
---@param x number
---@return number
function math.abs(x) end

---The largest value.
---@param x number
---@param ... number
---@return number
function math.max(x, ...) end

---The smallest value.
---@param x number
---@param ... number
---@return number
function math.min(x, ...) end

---A random float in [0, 1), or a random integer in [1, m] or [m, n].
---@param m? integer
---@param n? integer
---@return number
function math.random(m, n) end

---Seeds the random generator.
---@param x? integer
---@param y? integer
function math.randomseed(x, y) end

---The square root.
---@param x number
---@return number
function math.sqrt(x) end

---The remainder of x / y, rounded towards zero.
---@param x number
---@param y number
---@return number
function math.fmod(x, y) end

---The integral and the fractional part.
---@param x number
---@return number, number
function math.modf(x) end

---Converts to an integer, or nil when the value has no exact integer.
---@param x any
---@return integer|nil
function math.tointeger(x) end

---"integer", "float", or nil for anything that is not a number.
---@param x any
---@return "integer"|"float"|nil
function math.type(x) end

---e to the power of x.
---@param x number
---@return number
function math.exp(x) end

---The logarithm, natural or to a base.
---@param x number
---@param base? number
---@return number
function math.log(x, base) end

---The sine, in radians.
---@param x number
---@return number
function math.sin(x) end

---The cosine, in radians.
---@param x number
---@return number
function math.cos(x) end

---The tangent, in radians.
---@param x number
---@return number
function math.tan(x) end

---The arc tangent of y / x, in radians.
---@param y number
---@param x? number
---@return number
function math.atan(y, x) end

---The current time, or the time described by the table.
---@param table? table
---@return integer
function os.time(table) end

---Formats a date; "*t" returns a table and a leading "!" uses UTC.
---@param format? string
---@param time? integer
---@return string|table
function os.date(format, time) end

---The CPU time of the program in seconds.
---@return number
function os.clock() end

---The seconds between two times.
---@param t2 integer
---@param t1 integer
---@return number
function os.difftime(t2, t1) end

---The value of an environment variable, or nil.
---@param name string
---@return string|nil
function os.getenv(name) end

---Runs a shell command.
---@param command? string
---@return boolean|nil, string, integer
function os.execute(command) end

---Deletes a file.
---@param filename string
---@return boolean|nil, string|nil
function os.remove(filename) end

---Renames a file.
---@param old string
---@param new string
---@return boolean|nil, string|nil
function os.rename(old, new) end

---Opens a file, or returns nil and the error.
---@param filename string
---@param mode? "r"|"w"|"a"|"r+"|"w+"|"a+"|"rb"|"wb"|"ab"|"r+b"|"w+b"|"a+b"
---@return file*|nil, string|nil
function io.open(filename, mode) end

---Reads from the default input.
---@param ... "n"|"l"|"L"|"a"|integer
---@return any
function io.read(...) end

---Writes to the default output.
---@param ... string|number
---@return file*
function io.write(...) end

---Iterates over the lines of a file, or of the default input.
---@param filename? string
---@param ... "n"|"l"|"L"|"a"|integer
---@return function
function io.lines(filename, ...) end

---Closes a file, or the default output.
---@param file? file*
---@return boolean|nil
function io.close(file) end

---Creates a coroutine from a function.
---@param f function
---@return thread
function coroutine.create(f) end

---Starts or continues a coroutine; returns true and its values, or false and the error.
---@param co thread
---@param ... any
---@return boolean, any
function coroutine.resume(co, ...) end

---Suspends the running coroutine and hands the values to resume.
---@param ... any
---@return any
function coroutine.yield(...) end

---Creates a coroutine that is called like a function.
---@param f function
---@return function
)MLUA"),
            std::string_view(R"MLUA(function coroutine.wrap(f) end

---The status of a coroutine.
---@param co thread
---@return "running"|"suspended"|"normal"|"dead"
function coroutine.status(co) end

---The running coroutine and whether it is the main one.
---@return thread, boolean
function coroutine.running() end

---Information about a function or a stack level.
---@param thread? thread
---@param f function|integer
---@param what? string # letters of the fields: "n" "S" "l" "t" "u" "f" "L"
---@return table|nil
function debug.getinfo(thread, f, what) end

---A stack traceback as a string.
---@param thread? thread
---@param message? any
---@param level? integer
---@return string
function debug.traceback(thread, message, level) end

---The name and value of a local variable.
---@param thread? thread
---@param level integer
---@param index integer
---@return string|nil, any
function debug.getlocal(thread, level, index) end

---The name and value of an upvalue.
---@param f function
---@param index integer
---@return string|nil, any
function debug.getupvalue(f, index) end

---Sets an upvalue and returns its name.
---@param f function
---@param index integer
---@param value any
---@return string|nil
function debug.setupvalue(f, index, value) end

---Sets a debug hook.
---@param thread? thread
---@param hook function
---@param mask string # "c" calls, "r" returns, "l" lines
---@param count? integer
function debug.sethook(thread, hook, mask, count) end

---The metatable of a value, without `__metatable` protection.
---@param value any
---@return table|nil
function debug.getmetatable(value) end

---Sets the metatable of any value.
---@param value any
---@param table table|nil
---@return any
function debug.setmetatable(value, table) end

---Builds a UTF-8 string from code points.
---@param ... integer
---@return string
function utf8.char(...) end

---The number of UTF-8 characters from i to j, or nil and the position of the first invalid byte.
---@param s string
---@param i? integer
---@param j? integer
---@return integer|nil
function utf8.len(s, i, j) end

---The code points of the characters from i to j.
---@param s string
---@param i? integer
---@param j? integer
---@return integer
function utf8.codepoint(s, i, j) end
)MLUA"),
        };

        const std::array<CompatScript, 6> kScripts = {
            CompatScript{ "Defines.lua", std::span<const std::string_view>(kScript0) },
            CompatScript{ "Classes.lua", std::span<const std::string_view>(kScript1) },
            CompatScript{ "Lcl.lua", std::span<const std::string_view>(kScript2) },
            CompatScript{ "DotNet.lua", std::span<const std::string_view>(kScript3) },
            CompatScript{ "Extensions.lua", std::span<const std::string_view>(kScript4) },
            CompatScript{ "Functions.lua", std::span<const std::string_view>(kScript5) },
        };

        const std::array<CompatScript, 3> kManifoldScripts = {
            CompatScript{ "Manifold.lua", std::span<const std::string_view>(kManifold0) },
            CompatScript{ "RttiCompat.lua", std::span<const std::string_view>(kManifold1) },
            CompatScript{ "SigMakerCompat.lua", std::span<const std::string_view>(kManifold2) },
        };

        const std::array<CompatScript, 1> kDocScripts = {
            CompatScript{ "Standard.lua", std::span<const std::string_view>(kDoc0) },
        };
    }

    std::string CompatScript::Text() const
    {
        std::string text;
        for (std::string_view chunk : Chunks)
            text += chunk;
        return text;
    }

    std::span<const CompatScript> CompatScripts()
    {
        return kScripts;
    }

    std::span<const CompatScript> ManifoldScripts()
    {
        return kManifoldScripts;
    }

    std::span<const CompatScript> DocScripts()
    {
        return kDocScripts;
    }
}

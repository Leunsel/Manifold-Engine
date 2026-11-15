#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Manifold::Lua
{
    enum class MessageKind
    {
        Warning = 0,
        Error = 1,
        Information = 2,
        Confirmation = 3,
        Custom = 4
    };

    enum MessageButton
    {
        ButtonYes = 0,
        ButtonNo = 1,
        ButtonOk = 2,
        ButtonCancel = 3,
        ButtonAbort = 4,
        ButtonRetry = 5,
        ButtonIgnore = 6,
        ButtonAll = 7,
        ButtonNoToAll = 8,
        ButtonYesToAll = 9,
        ButtonHelp = 10,
        ButtonClose = 11
    };

    enum ModalResult
    {
        ResultNone = 0,
        ResultOk = 1,
        ResultCancel = 2,
        ResultAbort = 3,
        ResultRetry = 4,
        ResultIgnore = 5,
        ResultYes = 6,
        ResultNo = 7,
        ResultAll = 8,
        ResultNoToAll = 9,
        ResultYesToAll = 10,
        ResultClose = 11
    };

    struct DialogProvider
    {
        std::function<bool()> Available;
        std::function<std::optional<std::string>(const std::string& caption, const std::string& prompt, const std::string& value)> AskText;
        std::function<int(const std::string& title, const std::string& text, MessageKind kind, const std::vector<int>& buttons)> Message;
        std::function<std::pair<int, std::string>(const std::string& title, const std::string& caption, const std::vector<std::string>& items, bool allowCustom)> Select;
    };

    void SetDialogProvider(DialogProvider provider);
    void ClearDialogProvider();

    int ButtonResult(int button);
    const char* ButtonCaption(int button);

    bool AskText(const std::string& caption, const std::string& prompt, std::string& value);
    int AskMessage(const std::string& title, const std::string& text, MessageKind kind, std::vector<int> buttons);
    std::pair<int, std::string> AskSelection(const std::string& title, const std::string& caption, const std::vector<std::string>& items, bool allowCustom);
}

#include "Target/CodeCall.h"

#include "Asm/Assembler.h"
#include "Core/Text.h"
#include "Target/Process.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::size_t kCodeBytes = 0x800;
        constexpr std::size_t kResultOffset = kCodeBytes;
        constexpr std::size_t kTextOffset = kCodeBytes + 0x20;
        constexpr std::size_t kSavedStack = 0x10;

        constexpr const char* kRegisters64[16] = { "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15" };
        constexpr const char* kRegisters32[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

        bool Preserved(int index)
        {
            return index == 3 || index == 5 || index == 6 || index == 7 || index >= 12;
        }

        struct Emitter
        {
            bool Is64Bit = true;
            std::uint64_t Address = 0;
            std::vector<std::uint8_t> Code;
            Status Problem = Status::Success();

            void Line(const std::string& text)
            {
                if (!Problem)
                    return;
                Asm::AssembleOptions options;
                options.Is64Bit = Is64Bit;
                options.Address = Address + Code.size();
                const Asm::AssembleResult result = Asm::Assemble(text, options);
                if (!result.Ok)
                {
                    Problem = Status::Fail(ErrorCode::ThreadCreateFailed, "the call stub does not assemble at \"" + text + "\": " + result.Message);
                    return;
                }
                Code.insert(Code.end(), result.Bytes.begin(), result.Bytes.end());
            }

            void Bytes(std::initializer_list<std::uint8_t> bytes)
            {
                Code.insert(Code.end(), bytes.begin(), bytes.end());
            }
        };
    }

    Result<std::vector<std::uint8_t>> CodeCallStub(bool is64Bit, std::uint64_t address, std::uint64_t resultAddress, const CodeCallRequest& request,
        const std::vector<std::uint64_t>& values)
    {
        if (request.Function < 0x10000)
            return Status::Fail(ErrorCode::ThreadCreateFailed, "the address to call is not valid");
        if (values.size() != request.Arguments.size() || values.size() > kMaxCodeCallArguments)
            return Status::Fail(ErrorCode::ThreadCreateFailed, "a call takes at most " + std::to_string(kMaxCodeCallArguments) + " parameters");
        const int registerCount = is64Bit ? 16 : 8;
        const bool namedRegister = request.HasInstance && request.InstanceRegister >= 0;
        if (namedRegister && (request.InstanceRegister >= registerCount || request.InstanceRegister == 4))
            return Status::Fail(ErrorCode::ThreadCreateFailed, "register " + std::to_string(request.InstanceRegister) + " cannot carry the class instance");
        const bool keep = namedRegister && Preserved(request.InstanceRegister);

        Emitter e;
        e.Is64Bit = is64Bit;
        e.Address = address;
        if (is64Bit)
        {
            std::vector<std::uint64_t> all;
            std::vector<CodeCallKind> kinds;
            if (request.HasInstance && !namedRegister)
            {
                all.push_back(request.Instance);
                kinds.push_back(CodeCallKind::Integer);
            }
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                all.push_back(values[i]);
                kinds.push_back(request.Arguments[i].Kind);
            }
            const std::size_t onStack = all.size() > 4 ? all.size() - 4 : 0;
            std::size_t frame = 0x20 + onStack * 8;
            if ((8 + (keep ? 8 : 0) + frame) % 16 != 0)
                frame += 8;
            const char* instance = namedRegister ? kRegisters64[request.InstanceRegister] : nullptr;
            const char* target = namedRegister && request.InstanceRegister == 0 ? "r11" : "rax";
            if (keep)
                e.Line(std::string("push ") + instance);
            e.Line(Format("sub rsp,0x%zX", frame));
            for (std::size_t i = 4; i < all.size(); ++i)
            {
                e.Line(Format("mov rax,0x%llX", static_cast<unsigned long long>(all[i])));
                e.Line(Format("mov [rsp+0x%zX],rax", 0x20 + (i - 4) * 8));
            }
            static constexpr const char* kArgumentRegisters[4] = { "rcx", "rdx", "r8", "r9" };
            for (std::size_t i = 0; i < all.size() && i < 4; ++i)
            {
                e.Line(Format("mov %s,0x%llX", kArgumentRegisters[i], static_cast<unsigned long long>(all[i])));
                if (kinds[i] != CodeCallKind::Float && kinds[i] != CodeCallKind::Double)
                    continue;
                switch (i)
                {
                case 0: e.Bytes({ 0x66, 0x48, 0x0F, 0x6E, 0xC1 }); break;
                case 1: e.Bytes({ 0x66, 0x48, 0x0F, 0x6E, 0xCA }); break;
                case 2: e.Bytes({ 0x66, 0x49, 0x0F, 0x6E, 0xD0 }); break;
                default: e.Bytes({ 0x66, 0x49, 0x0F, 0x6E, 0xD9 }); break;
                }
            }
            if (namedRegister)
                e.Line(Format("mov %s,0x%llX", instance, static_cast<unsigned long long>(request.Instance)));
            e.Line(Format("mov %s,0x%llX", target, static_cast<unsigned long long>(request.Function)));
            e.Line(std::string("call ") + target);
            e.Line(Format("mov rcx,0x%llX", static_cast<unsigned long long>(resultAddress)));
            e.Line("mov [rcx],rax");
            e.Bytes({ 0x66, 0x0F, 0xD6, 0x41, 0x08 });
            e.Line(Format("add rsp,0x%zX", frame));
            if (keep)
                e.Line(std::string("pop ") + instance);
            e.Line("xor eax,eax");
            e.Line("ret");
        }
        else
        {
            const int carrier = request.HasInstance ? (namedRegister ? request.InstanceRegister : 1) : -1;
            const char* target = carrier == 0 ? "edx" : "eax";
            if (keep)
                e.Line(std::string("push ") + kRegisters32[carrier]);
            // esp is saved and put back after the call, so a function with the other calling
            // convention than the script said (cdecl vs stdcall) cannot unbalance the stack.
            e.Line(Format("mov [0x%X],esp", static_cast<std::uint32_t>(resultAddress + kSavedStack)));
            for (std::size_t i = values.size(); i-- > 0;)
            {
                if (request.Arguments[i].Kind == CodeCallKind::Double)
                {
                    e.Line(Format("push 0x%X", static_cast<std::uint32_t>(values[i] >> 32)));
                }
                e.Line(Format("push 0x%X", static_cast<std::uint32_t>(values[i])));
            }
            if (carrier >= 0)
                e.Line(Format("mov %s,0x%X", kRegisters32[carrier], static_cast<std::uint32_t>(request.Instance)));
            e.Line(Format("mov %s,0x%X", target, static_cast<std::uint32_t>(request.Function)));
            e.Line(std::string("call ") + target);
            e.Line(Format("mov esp,[0x%X]", static_cast<std::uint32_t>(resultAddress + kSavedStack)));
            e.Line(Format("mov [0x%X],eax", static_cast<std::uint32_t>(resultAddress)));
            e.Line(Format("mov [0x%X],edx", static_cast<std::uint32_t>(resultAddress + 4)));
            if (keep)
                e.Line(std::string("pop ") + kRegisters32[carrier]);
            e.Line("xor eax,eax");
            e.Line("ret 4");
        }
        if (!e.Problem)
            return e.Problem;
        if (e.Code.size() > kCodeBytes)
            return Status::Fail(ErrorCode::ThreadCreateFailed, "the call stub is too large");
        return e.Code;
    }

    Result<std::uint64_t> CallCode(const Process& process, const CodeCallRequest& request)
    {
        if (request.Arguments.size() > kMaxCodeCallArguments)
            return Status::Fail(ErrorCode::ThreadCreateFailed, "a call takes at most " + std::to_string(kMaxCodeCallArguments) + " parameters");
        std::vector<std::uint8_t> text;
        std::vector<std::size_t> textAt(request.Arguments.size(), 0);
        for (std::size_t i = 0; i < request.Arguments.size(); ++i)
        {
            const CodeCallArgument& argument = request.Arguments[i];
            if (argument.Kind != CodeCallKind::Text && argument.Kind != CodeCallKind::WideText)
                continue;
            while (text.size() % 8 != 0)
                text.push_back(0);
            textAt[i] = text.size();
            if (argument.Kind == CodeCallKind::Text)
            {
                text.insert(text.end(), argument.Text.begin(), argument.Text.end());
                text.push_back(0);
            }
            else
            {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(argument.Wide.c_str());
                text.insert(text.end(), bytes, bytes + (argument.Wide.size() + 1) * sizeof(wchar_t));
            }
        }
        if (text.size() > 0x100000)
            return Status::Fail(ErrorCode::ThreadCreateFailed, "the text parameters are too large");

        const Result<Address> allocated = process.Allocate(kTextOffset + text.size() + 16, PAGE_EXECUTE_READWRITE);
        if (!allocated)
            return allocated.Error();
        const std::uint64_t block = *allocated;
        std::vector<std::uint64_t> values(request.Arguments.size());
        for (std::size_t i = 0; i < request.Arguments.size(); ++i)
        {
            const CodeCallArgument& argument = request.Arguments[i];
            values[i] = argument.Kind == CodeCallKind::Text || argument.Kind == CodeCallKind::WideText ? block + kTextOffset + textAt[i] : argument.Value;
        }
        const Result<std::vector<std::uint8_t>> stub = CodeCallStub(process.Is64Bit(), block, block + kResultOffset, request, values);
        if (!stub)
        {
            process.Free(block);
            return stub.Error();
        }
        Status written = process.Write(block, stub->data(), stub->size());
        if (written && !text.empty())
            written = process.Write(block + kTextOffset, text.data(), text.size());
        if (!written)
        {
            process.Free(block);
            return written;
        }
        const Status ran = process.CreateThread(block, 0, request.Wait, request.TimeoutMilliseconds);
        if (!ran)
        {
            if (ran.Code != ErrorCode::ThreadTimedOut)
                process.Free(block);
            return ran;
        }
        if (!request.Wait || Process::DryRun())
        {
            if (Process::DryRun())
                process.Free(block);
            return std::uint64_t(0);
        }
        std::uint64_t result = 0;
        const bool read = process.ReadExact(block + kResultOffset, &result, process.Is64Bit() ? 8 : 4);
        process.Free(block);
        if (!read)
            return Status::Fail(ErrorCode::ThreadCreateFailed, "the result of the call could not be read");
        return result;
    }
}

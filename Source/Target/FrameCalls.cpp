#include "Target/FrameCalls.h"

#include "Asm/Assembler.h"
#include "Core/Text.h"
#include "Target/Process.h"

#include <algorithm>
#include <map>
#include <set>
#include <thread>

#include <windows.h>

namespace Manifold::Target
{
    namespace
    {
        constexpr std::uint32_t kRequest = 0x00;
        constexpr std::uint32_t kDone = 0x04;
        constexpr std::uint32_t kCount = 0x08;
        constexpr std::uint32_t kNext = 0x0C;
        constexpr std::uint32_t kAbort = 0x10;
        constexpr std::uint32_t kRetired = 0x14;
        constexpr std::uint32_t kBusy = 0x18;
        constexpr std::uint32_t kPerFrame = 0x1C;
        constexpr std::uint32_t kFrames = 0x20;
        constexpr std::uint32_t kThread = 0x28;
        constexpr std::uint32_t kEntries = 0x30;
        constexpr std::uint32_t kLeft = 0x38;
        constexpr std::uint32_t kOriginals = 0x40;
        constexpr std::uint32_t kControlSize = 0x80;
        constexpr std::uint32_t kEntrySize = 0x40;
        constexpr std::uint32_t kEntryResult = 0x28;
        constexpr std::uint32_t kEntryRefs = 0x30;
        constexpr std::uint32_t kWorkerBudget = 0x7FFFFFFF;
        constexpr auto kAbortWait = std::chrono::milliseconds(250);
        constexpr auto kUnhookWait = std::chrono::milliseconds(500);

        struct Assembled
        {
            std::vector<std::uint8_t> Bytes;
            std::map<std::string, std::uint64_t, std::less<>> Labels;
        };

        Result<Assembled> AssembleLines(bool is64, std::uint64_t address, const std::vector<std::string>& lines)
        {
            Assembled out;
            std::set<std::string, std::less<>> declared;
            for (const std::string& line : lines)
            {
                if (line.ends_with(':'))
                    declared.insert(line.substr(0, line.size() - 1));
            }
            for (int pass = 0; pass < 8; ++pass)
            {
                std::vector<std::uint8_t> code;
                bool settled = true;
                for (const std::string& line : lines)
                {
                    if (line.ends_with(':'))
                    {
                        const std::uint64_t at = address + code.size();
                        const auto [label, added] = out.Labels.try_emplace(line.substr(0, line.size() - 1), at);
                        settled = settled && !added && label->second == at;
                        label->second = at;
                        continue;
                    }
                    Asm::AssembleOptions options;
                    options.Is64Bit = is64;
                    options.Address = address + code.size();
                    options.AllowPending = true;
                    options.Resolve = [&out, &declared](std::string_view name)
                        {
                            if (const auto found = out.Labels.find(name); found != out.Labels.end())
                                return Asm::SymbolValue{ Asm::SymbolState::Known, found->second };
                            return Asm::SymbolValue{ declared.count(name) ? Asm::SymbolState::Pending : Asm::SymbolState::Unknown, 0 };
                        };
                    const Asm::AssembleResult result = Asm::Assemble(line, options);
                    if (!result.Ok)
                        return Status::Fail(ErrorCode::ThreadCreateFailed, "the frame stub does not assemble at \"" + line + "\": " + result.Message);
                    settled = settled && !result.UsedPending;
                    code.insert(code.end(), result.Bytes.begin(), result.Bytes.end());
                }
                if (settled)
                {
                    out.Bytes = std::move(code);
                    return out;
                }
            }
            return Status::Fail(ErrorCode::ThreadCreateFailed, "the frame stub's jumps do not settle");
        }

        void Add(std::vector<std::string>& lines, std::initializer_list<std::string> more)
        {
            lines.insert(lines.end(), more.begin(), more.end());
        }

        std::vector<std::string> Lines64(std::uint64_t control)
        {
            std::vector<std::string> lines;
            for (int i = 0; i < FrameCalls::kHookSlots; ++i)
            {
                Add(lines, {
                    Format("thunk%d:", i),
                    "push rcx", "push rdx", "push r8", "push r9",
                    "sub rsp,0x28",
                    Format("mov rax,0x%llX", static_cast<unsigned long long>(control)),
                    Format("mov ecx,[rax+0x%X]", kPerFrame),
                    "call body",
                    "add rsp,0x28",
                    "pop r9", "pop r8", "pop rdx", "pop rcx",
                    Format("mov rax,0x%llX", static_cast<unsigned long long>(control + kOriginals + 8ull * static_cast<unsigned>(i))),
                    "jmp qword ptr [rax]",
                });
            }
            Add(lines, {
                "body:",
                "push rbx", "push rsi", "push rdi", "push r12", "push rbp",
                "sub rsp,0x20",
                Format("mov rbx,0x%llX", static_cast<unsigned long long>(control)),
                "mov r12d,ecx",
                Format("inc qword ptr [rbx+0x%X]", kFrames),
                "mov eax,gs:[0x48]",
                Format("mov [rbx+0x%X],eax", kThread),
                Format("cmp dword ptr [rbx+0x%X],0", kRetired),
                "jne bail",
                "jmp claim",
                "direct:",
                "push rbx", "push rsi", "push rdi", "push r12", "push rbp",
                "sub rsp,0x20",
                Format("mov rbx,0x%llX", static_cast<unsigned long long>(control)),
                "mov r12d,ecx",
                "claim:",
                "mov eax,1",
                Format("xchg dword ptr [rbx+0x%X],eax", kBusy),
                "test eax,eax",
                "jnz bail",
                Format("mov eax,[rbx+0x%X]", kRequest),
                Format("cmp eax,[rbx+0x%X]", kDone),
                "je release",
                Format("mov rsi,[rbx+0x%X]", kEntries),
                "next:",
                Format("cmp dword ptr [rbx+0x%X],0", kAbort),
                "jne finish",
                Format("mov eax,[rbx+0x%X]", kNext),
                Format("cmp eax,[rbx+0x%X]", kCount),
                "jae finish",
                "test r12d,r12d",
                "jz release",
                "dec r12d",
                "mov edi,eax",
                "shl rdi,6",
                "add rdi,rsi",
            });
            static const char* const kRegisters[] = { "rcx", "rdx", "r8", "r9" };
            for (int i = 0; i < 4; ++i)
            {
                Add(lines, {
                    Format("mov %s,[rdi+0x%X]", kRegisters[i], 8 + 8 * i),
                    Format("movzx eax,word ptr [rdi+0x%X]", kEntryRefs + 2 * i),
                    "test eax,eax",
                    Format("jz arg%d", i),
                    "dec eax",
                    "shl rax,6",
                    Format("mov rax,[rsi+rax+0x%X]", kEntryResult),
                    "test rax,rax",
                    "jz skip",
                    Format("add %s,rax", kRegisters[i]),
                    Format("arg%d:", i),
                });
            }
            Add(lines, {
                "call qword ptr [rdi]",
                Format("mov [rdi+0x%X],rax", kEntryResult),
                "jmp advance",
                "skip:",
                Format("mov qword ptr [rdi+0x%X],0", kEntryResult),
                "advance:",
                Format("inc dword ptr [rbx+0x%X]", kNext),
                "jmp next",
                "finish:",
                Format("mov eax,[rbx+0x%X]", kRequest),
                Format("mov [rbx+0x%X],eax", kDone),
                "release:",
                Format("mov dword ptr [rbx+0x%X],0", kBusy),
                "bail:",
                "add rsp,0x20",
                "pop rbp", "pop r12", "pop rdi", "pop rsi", "pop rbx",
                "ret",
                "store:",
                "mov [rcx],rdx",
                "mov rax,rdx",
                "ret",
                "load:",
                "mov rax,[rcx]",
                "ret",
                "copy:",
                "push rsi", "push rdi",
                "mov rdi,rcx",
                "mov rsi,rdx",
                "mov rcx,r8",
                "rep movsb",
                "pop rdi", "pop rsi",
                "ret",
                "text:",
                "push rsi", "push rdi",
                "mov eax,[rdx]",
                "test eax,eax",
                "jns textpos",
                "xor eax,eax",
                "textpos:",
                "mov [rcx+4],eax",
                "cmp eax,r8d",
                "jbe textfit",
                "mov eax,r8d",
                "textfit:",
                "mov [rcx],eax",
                "lea rdi,[rcx+8]",
                "lea rsi,[rdx+4]",
                "mov ecx,eax",
                "add ecx,ecx",
                "rep movsb",
                "pop rdi", "pop rsi",
                "ret",
            });
            return lines;
        }

        std::vector<std::string> Lines32(std::uint64_t control)
        {
            const unsigned base = static_cast<unsigned>(control);
            std::vector<std::string> lines;
            for (int i = 0; i < FrameCalls::kHookSlots; ++i)
            {
                Add(lines, {
                    Format("thunk%d:", i),
                    "pushad",
                    Format("push dword ptr [0x%X]", base + kPerFrame),
                    "call body",
                    "add esp,4",
                    "popad",
                    Format("jmp dword ptr [0x%X]", base + kOriginals + 8u * static_cast<unsigned>(i)),
                });
            }
            Add(lines, {
                "body:",
                "push ebp", "push ebx", "push esi", "push edi",
                Format("mov ebx,0x%X", base),
                Format("add dword ptr [ebx+0x%X],1", kFrames),
                Format("adc dword ptr [ebx+0x%X],0", kFrames + 4),
                "mov eax,fs:[0x24]",
                Format("mov [ebx+0x%X],eax", kThread),
                Format("cmp dword ptr [ebx+0x%X],0", kRetired),
                "jne bail",
                "jmp claim",
                "direct:",
                "push ebp", "push ebx", "push esi", "push edi",
                Format("mov ebx,0x%X", base),
                "claim:",
                "mov ebp,esp",
                "mov eax,1",
                Format("xchg dword ptr [ebx+0x%X],eax", kBusy),
                "test eax,eax",
                "jnz bail",
                "mov eax,[ebp+0x14]",
                Format("mov [ebx+0x%X],eax", kLeft),
                Format("mov eax,[ebx+0x%X]", kRequest),
                Format("cmp eax,[ebx+0x%X]", kDone),
                "je release",
                Format("mov esi,[ebx+0x%X]", kEntries),
                "next:",
                Format("cmp dword ptr [ebx+0x%X],0", kAbort),
                "jne finish",
                Format("mov eax,[ebx+0x%X]", kNext),
                Format("cmp eax,[ebx+0x%X]", kCount),
                "jae finish",
                Format("cmp dword ptr [ebx+0x%X],0", kLeft),
                "je release",
                Format("dec dword ptr [ebx+0x%X]", kLeft),
                "mov edi,eax",
                "shl edi,6",
                "add edi,esi",
            });
            for (int i = 3; i >= 0; --i)
            {
                Add(lines, {
                    Format("mov ecx,[edi+0x%X]", 8 + 8 * i),
                    Format("movzx eax,word ptr [edi+0x%X]", kEntryRefs + 2 * i),
                    "test eax,eax",
                    Format("jz arg%d", i),
                    "dec eax",
                    "shl eax,6",
                    Format("mov eax,[esi+eax+0x%X]", kEntryResult),
                    "test eax,eax",
                    "jz skip",
                    "add ecx,eax",
                    Format("arg%d:", i),
                    "push ecx",
                });
            }
            Add(lines, {
                "call dword ptr [edi]",
                "mov esp,ebp",
                Format("mov [edi+0x%X],eax", kEntryResult),
                Format("mov dword ptr [edi+0x%X],0", kEntryResult + 4),
                "jmp advance",
                "skip:",
                "mov esp,ebp",
                Format("mov dword ptr [edi+0x%X],0", kEntryResult),
                Format("mov dword ptr [edi+0x%X],0", kEntryResult + 4),
                "advance:",
                Format("inc dword ptr [ebx+0x%X]", kNext),
                "jmp next",
                "finish:",
                Format("mov eax,[ebx+0x%X]", kRequest),
                Format("mov [ebx+0x%X],eax", kDone),
                "release:",
                Format("mov dword ptr [ebx+0x%X],0", kBusy),
                "bail:",
                "pop edi", "pop esi", "pop ebx", "pop ebp",
                "ret",
                "store:",
                "mov eax,[esp+4]",
                "mov edx,[esp+8]",
                "mov [eax],edx",
                "mov eax,edx",
                "ret",
                "load:",
                "mov eax,[esp+4]",
                "mov eax,[eax]",
                "ret",
                "copy:",
                "push esi", "push edi",
                "mov edi,[esp+0xC]",
                "mov esi,[esp+0x10]",
                "mov ecx,[esp+0x14]",
                "rep movsb",
                "pop edi", "pop esi",
                "ret",
                "text:",
                "push esi", "push edi",
                "mov edi,[esp+0xC]",
                "mov esi,[esp+0x10]",
                "mov eax,[esi]",
                "test eax,eax",
                "jns textpos",
                "xor eax,eax",
                "textpos:",
                "mov [edi+4],eax",
                "cmp eax,[esp+0x14]",
                "jbe textfit",
                "mov eax,[esp+0x14]",
                "textfit:",
                "mov [edi],eax",
                "add edi,8",
                "add esi,4",
                "mov ecx,eax",
                "add ecx,ecx",
                "rep movsb",
                "pop edi", "pop esi",
                "ret",
            });
            return lines;
        }
    }

    int FrameBatch::Call(std::uint64_t function, FrameArg a0, FrameArg a1, FrameArg a2, FrameArg a3)
    {
        if (m_calls.size() >= m_maxCalls || !function)
        {
            m_overflow = true;
            return -1;
        }
        Entry entry;
        entry.Function = function;
        const FrameArg args[4] = { a0, a1, a2, a3 };
        for (int i = 0; i < 4; ++i)
        {
            entry.Args[i] = args[i].Value;
            if (args[i].From >= 0 && args[i].From < static_cast<int>(m_calls.size()))
                entry.Refs[i] = static_cast<std::uint16_t>(args[i].From + 1);
        }
        m_calls.push_back(entry);
        return static_cast<int>(m_calls.size()) - 1;
    }

    std::uint64_t FrameBatch::Cell(std::size_t bytes, const void* initial)
    {
        const std::size_t aligned = (std::max<std::size_t>(bytes, 1) + 15) & ~std::size_t(15);
        if (m_data.size() + aligned > m_maxBytes)
        {
            m_overflow = true;
            return 0;
        }
        const std::size_t at = m_data.size();
        m_data.resize(at + aligned, 0);
        if (initial)
            std::memcpy(m_data.data() + at, initial, bytes);
        return m_base + at;
    }

    bool FrameBatch::Fits(std::size_t calls, std::size_t bytes) const
    {
        return m_calls.size() + calls <= m_maxCalls && m_data.size() + bytes <= m_maxBytes;
    }

    std::uint64_t FrameBatch::Result(int call) const
    {
        if (call < 0 || call >= static_cast<int>(m_calls.size()))
            return 0;
        const std::uint64_t value = m_calls[static_cast<std::size_t>(call)].Result;
        return m_pointer == 4 ? value & 0xFFFFFFFFull : value;
    }

    bool FrameBatch::Read(std::uint64_t cell, void* out, std::size_t size) const
    {
        if (cell < m_base || cell - m_base > m_data.size() || size > m_data.size() - static_cast<std::size_t>(cell - m_base))
        {
            std::memset(out, 0, size);
            return false;
        }
        std::memcpy(out, m_data.data() + static_cast<std::size_t>(cell - m_base), size);
        return true;
    }

    std::uint64_t FrameBatch::Pointer(std::uint64_t cell) const
    {
        std::uint64_t value = 0;
        Read(cell, &value, static_cast<std::size_t>(m_pointer));
        return value;
    }

    FrameCalls::~FrameCalls()
    {
        Unhook();
    }

    Result<std::vector<std::uint8_t>> FrameCalls::Code(bool is64, std::uint64_t address, std::uint64_t control, std::vector<std::uint64_t>* labels)
    {
        Result<Assembled> assembled = AssembleLines(is64, address, is64 ? Lines64(control) : Lines32(control));
        if (!assembled)
            return assembled.Error();
        if (labels)
        {
            labels->clear();
            for (int i = 0; i < kHookSlots; ++i)
                labels->push_back(assembled->Labels[Format("thunk%d", i)]);
            for (const char* name : { "direct", "store", "load", "copy", "text" })
                labels->push_back(assembled->Labels[name]);
        }
        return std::move(assembled->Bytes);
    }

    Status FrameCalls::Start(std::shared_ptr<Process> process, WorkerCall worker)
    {
        if (m_control)
            return Status::Success();
        if (!process || !process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        const Result<Address> code = process->Allocate(0x1000, PAGE_EXECUTE_READWRITE);
        const Result<Address> control = code ? process->Allocate(0x1000, PAGE_READWRITE) : Result<Address>(code.Error());
        const Result<Address> entries = control ? process->Allocate(kMaxCalls * kEntrySize, PAGE_READWRITE) : Result<Address>(control.Error());
        const Result<Address> data = entries ? process->Allocate(kDataSize, PAGE_READWRITE) : Result<Address>(entries.Error());
        if (!data)
            return data.Error();
        const bool is64 = process->Is64Bit();
        if (!is64 && std::max({ *code + 0x1000, *control + 0x1000, *entries + kMaxCalls * kEntrySize, *data + kDataSize }) > 0x100000000ull)
            return Status::Fail(ErrorCode::ThreadCreateFailed, "the frame stub's pages lie above 4 GB in a 32-bit target");
        std::vector<std::uint64_t> labels;
        const Result<std::vector<std::uint8_t>> bytes = Code(is64, *code, *control, &labels);
        if (!bytes)
            return bytes.Error();
        if (bytes->size() > 0x1000 || labels.size() != static_cast<std::size_t>(kHookSlots) + 5)
            return Status::Fail(ErrorCode::WriteFailed, "the frame stub does not fit its page");
        std::vector<std::uint8_t> block(kControlSize, 0);
        const std::uint32_t perFrame = kCallsPerFrame;
        std::memcpy(block.data() + kPerFrame, &perFrame, sizeof(perFrame));
        const std::uint64_t entriesAt = *entries;
        std::memcpy(block.data() + kEntries, &entriesAt, sizeof(entriesAt));
        const std::uint32_t retired = 1;
        std::memcpy(block.data() + kRetired, &retired, sizeof(retired));
        if (!process->Write(*control, block.data(), block.size()) || !process->Write(*code, bytes->data(), bytes->size()))
            return Status::Fail(ErrorCode::WriteFailed, "could not set up the frame stub");
        ::FlushInstructionCache(process->Handle(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(*code)), bytes->size());
        m_process = std::move(process);
        m_worker = std::move(worker);
        m_code = *code;
        m_control = *control;
        m_entries = *entries;
        m_data = *data;
        for (int i = 0; i < kHookSlots; ++i)
            m_thunks[i] = labels[static_cast<std::size_t>(i)];
        m_direct = labels[kHookSlots];
        m_store = labels[kHookSlots + 1];
        m_load = labels[kHookSlots + 2];
        m_copy = labels[kHookSlots + 3];
        m_text = labels[kHookSlots + 4];
        m_hooksUsed = 0;
        m_hooked = false;
        return Status::Success();
    }

    Status FrameCalls::Hook(std::uint64_t slot, std::uint64_t expected)
    {
        if (!m_control || !m_process)
            return Status::Fail(ErrorCode::NoProcess);
        if (m_hooked)
            return Status::Fail(ErrorCode::MonoCallFailed, "the frame stub is already in place");
        int index = 0;
        while (index < m_hooksUsed && m_hookSlots[index] != slot)
            ++index;
        if (index >= kHookSlots)
            return Status::Fail(ErrorCode::MonoCallFailed, "the frame stub has no free slot left");
        Flush();
        if (Pending())
            return Status::Fail(ErrorCode::MonoCallFailed, "an earlier request is still waiting for the game's main thread");
        const auto region = m_process->RegionAt(slot);
        if (!region || !region->Committed() || !region->Writable() || region->Guarded())
            return Status::Fail(ErrorCode::MonoCallFailed, "the method's entry at " + Hex(slot) + " is not writable");
        const std::uint64_t current = m_process->ReadPointer(slot).value_or(0);
        if (!expected || current != expected)
            return Status::Fail(ErrorCode::MonoCallFailed, "the method's entry at " + Hex(slot) + " does not hold its code address");
        const std::uint64_t thunk = m_thunks[index];
        const std::uint32_t live = 0;
        if (!m_process->Write(m_control + kOriginals + 8ull * static_cast<unsigned>(index), &expected, sizeof(expected)) ||
            !m_process->Write(m_control + kRetired, &live, sizeof(live)))
            return Status::Fail(ErrorCode::WriteFailed);
        if (index == m_hooksUsed)
        {
            m_hookSlots[index] = slot;
            ++m_hooksUsed;
        }
        const bool stored = m_worker && m_worker(m_store, slot, thunk).has_value();
        if (!stored && !m_process->Write(slot, &thunk, static_cast<std::size_t>(m_process->PointerSize())))
        {
            const std::uint32_t retired = 1;
            m_process->Write(m_control + kRetired, &retired, sizeof(retired));
            return Status::Fail(ErrorCode::WriteFailed, "could not put the frame stub in place");
        }
        m_hooked = true;
        m_hookSlot = slot;
        m_hookOriginal = expected;
        m_hookThunk = thunk;
        return Status::Success();
    }

    void FrameCalls::Unhook()
    {
        if (!m_hooked || !m_process)
            return;
        m_hooked = false;
        if (!m_process->Alive())
            return;
        const std::uint32_t retired = 1;
        m_process->Write(m_control + kRetired, &retired, sizeof(retired));
        if (m_process->ReadPointer(m_hookSlot).value_or(0) == m_hookThunk)
        {
            const bool stored = m_worker && m_worker(m_store, m_hookSlot, m_hookOriginal).has_value();
            if (!stored)
                m_process->Write(m_hookSlot, &m_hookOriginal, static_cast<std::size_t>(m_process->PointerSize()));
        }
        const auto deadline = std::chrono::steady_clock::now() + kUnhookWait;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (m_process->ReadValue<std::uint32_t>(m_control + kBusy).value_or(0) == 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void FrameCalls::Forget()
    {
        m_hooked = false;
        m_process.reset();
        m_worker = {};
        m_code = m_control = m_entries = m_data = 0;
        m_hooksUsed = 0;
    }

    void FrameCalls::Flush()
    {
        if (m_hooked || !m_worker || !Pending())
            return;
        const std::uint32_t abort = 1;
        m_process->Write(m_control + kAbort, &abort, sizeof(abort));
        m_worker(m_direct, kWorkerBudget, 0);
    }

    std::uint64_t FrameCalls::Frames() const
    {
        return m_process && m_control ? m_process->ReadValue<std::uint64_t>(m_control + kFrames).value_or(0) : 0;
    }

    std::uint32_t FrameCalls::Thread() const
    {
        return m_process && m_control ? m_process->ReadValue<std::uint32_t>(m_control + kThread).value_or(0) : 0;
    }

    bool FrameCalls::Done() const
    {
        std::uint32_t state[2] = { 0, 1 };
        return m_process && m_process->ReadExact(m_control + kRequest, state, sizeof(state)) && state[0] == state[1];
    }

    bool FrameCalls::Pending() const
    {
        return m_process && m_control && !Done();
    }

    FrameBatch FrameCalls::Batch() const
    {
        FrameBatch batch;
        batch.m_base = m_data;
        batch.m_maxCalls = m_control ? kMaxCalls : 0;
        batch.m_maxBytes = m_control ? kDataSize : 0;
        batch.m_pointer = m_process ? m_process->PointerSize() : 8;
        return batch;
    }

    Status FrameCalls::Post(FrameBatch& batch)
    {
        if (!m_control || !m_process || !m_process->Alive())
            return Status::Fail(ErrorCode::NoProcess);
        if (batch.m_overflow || batch.m_base != m_data)
            return Status::Fail(ErrorCode::MonoCallFailed, "the request is too large for one batch");
        std::uint32_t state[2] = { 0, 1 };
        if (!m_process->ReadExact(m_control + kRequest, state, sizeof(state)))
            return Status::Fail(ErrorCode::ReadFailed);
        if (state[0] != state[1])
            return Status::Fail(ErrorCode::MonoCallFailed, "an earlier request is still waiting for the game's main thread (is the game paused or minimised?)");
        if (!batch.m_data.empty() && !m_process->Write(m_data, batch.m_data.data(), batch.m_data.size()))
            return Status::Fail(ErrorCode::WriteFailed);
        if (!batch.m_calls.empty() && !m_process->Write(m_entries, batch.m_calls.data(), batch.m_calls.size() * sizeof(FrameBatch::Entry)))
            return Status::Fail(ErrorCode::WriteFailed);
        const std::uint32_t header[3] = { static_cast<std::uint32_t>(batch.m_calls.size()), 0, 0 };
        if (!m_process->Write(m_control + kCount, header, sizeof(header)))
            return Status::Fail(ErrorCode::WriteFailed);
        const std::uint32_t request = state[1] + 1;
        if (!m_process->Write(m_control + kRequest, &request, sizeof(request)))
            return Status::Fail(ErrorCode::WriteFailed);
        return Status::Success();
    }

    Status FrameCalls::Collect(FrameBatch& batch)
    {
        if (!batch.m_calls.empty() && !m_process->ReadExact(m_entries, batch.m_calls.data(), batch.m_calls.size() * sizeof(FrameBatch::Entry)))
            return Status::Fail(ErrorCode::ReadFailed);
        if (!batch.m_data.empty() && !m_process->ReadExact(m_data, batch.m_data.data(), batch.m_data.size()))
            return Status::Fail(ErrorCode::ReadFailed);
        return Status::Success();
    }

    Result<FrameCalls::Ran> FrameCalls::Run(FrameBatch& batch, std::chrono::milliseconds timeout, const std::function<bool()>& aborted)
    {
        if (!m_hooked)
            return Status::Fail(ErrorCode::MonoCallFailed, "the frame stub is not in the game");
        const std::uint64_t before = Frames();
        m_executed = 0;
        if (Status posted = Post(batch); !posted)
            return posted;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string failure;
        for (std::uint64_t spins = 0; failure.empty(); ++spins)
        {
            if (Done())
                break;
            if (!m_process->Alive())
                return Status::Fail(ErrorCode::DebugTargetExited);
            if (aborted && aborted())
                failure = "cancelled";
            else if (std::chrono::steady_clock::now() > deadline)
                failure = "the game's main thread did not answer within " + std::to_string(timeout.count()) + " ms (is the game paused or minimised?)";
            else if (spins < 200)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!failure.empty())
        {
            const std::uint32_t abort = 1;
            m_process->Write(m_control + kAbort, &abort, sizeof(abort));
            const auto grace = std::chrono::steady_clock::now() + kAbortWait;
            while (!Done() && std::chrono::steady_clock::now() < grace && m_process->Alive())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            m_executed = std::min<std::size_t>(batch.m_calls.size(), static_cast<std::size_t>(m_process->ReadValue<std::uint32_t>(m_control + kNext).value_or(0)) + 1);
            Collect(batch);
            return Status::Fail(ErrorCode::MonoCallFailed, failure);
        }
        m_executed = batch.m_calls.size();
        if (Status collected = Collect(batch); !collected)
            return collected;
        Ran ran;
        ran.Frames = Frames() - before;
        return ran;
    }

    Status FrameCalls::RunOnWorker(FrameBatch& batch)
    {
        if (m_hooked)
            return Status::Fail(ErrorCode::MonoCallFailed, "the frame stub is still in the game");
        if (!m_worker)
            return Status::Fail(ErrorCode::NoProcess);
        Flush();
        if (Status posted = Post(batch); !posted)
            return posted;
        if (!m_worker(m_direct, kWorkerBudget, 0).has_value() || !Done())
        {
            const std::uint32_t abort = 1;
            m_process->Write(m_control + kAbort, &abort, sizeof(abort));
            return Status::Fail(ErrorCode::MonoCallFailed, "the worker did not run the request");
        }
        return Collect(batch);
    }
}

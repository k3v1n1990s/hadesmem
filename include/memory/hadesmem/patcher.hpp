// Copyright (C) 2010-2014 Joshua Boyce.
// See the file COPYING for copying permission.

#pragma once

#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#include <hadesmem/detail/warning_disable_prefix.hpp>
#include <udis86.h>
#include <hadesmem/detail/warning_disable_suffix.hpp>

#include <windows.h>

#include <hadesmem/alloc.hpp>
#include <hadesmem/detail/alias_cast.hpp>
#include <hadesmem/detail/assert.hpp>
#include <hadesmem/detail/scope_warden.hpp>
#include <hadesmem/detail/srw_lock.hpp>
#include <hadesmem/detail/thread_aux.hpp>
#include <hadesmem/detail/trace.hpp>
#include <hadesmem/detail/type_traits.hpp>
#include <hadesmem/detail/winternl.hpp>
#include <hadesmem/error.hpp>
#include <hadesmem/flush.hpp>
#include <hadesmem/process.hpp>
#include <hadesmem/read.hpp>
#include <hadesmem/thread.hpp>
#include <hadesmem/thread_list.hpp>
#include <hadesmem/thread_helpers.hpp>
#include <hadesmem/write.hpp>

namespace hadesmem
{
namespace detail
{
inline void VerifyPatchThreads(DWORD pid, void* target, std::size_t len)
{
  ThreadList threads{pid};
  for (auto const& thread_entry : threads)
  {
    if (thread_entry.GetId() == ::GetCurrentThreadId())
    {
      continue;
    }

    if (IsExecutingInRange(
          thread_entry, target, static_cast<std::uint8_t*>(target) + len))
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{"Thread is currently executing patch target."});
    }
  }
}
}

class PatchRaw
{
public:
  explicit PatchRaw(Process const& process,
                    void* target,
                    std::vector<std::uint8_t> const& data)
    : process_{&process}, target_{target}, data_(data)
  {
  }

  explicit PatchRaw(Process&& process,
                    PVOID target,
                    std::vector<std::uint8_t> const& data) = delete;

  PatchRaw(PatchRaw const& other) = delete;

  PatchRaw& operator=(PatchRaw const& other) = delete;

  PatchRaw(PatchRaw&& other)
    : process_{other.process_},
      applied_{other.applied_},
      target_{other.target_},
      data_(std::move(other.data_)),
      orig_(std::move(other.orig_))
  {
    other.process_ = nullptr;
    other.applied_ = false;
    other.target_ = nullptr;
  }

  PatchRaw& operator=(PatchRaw&& other)
  {
    RemoveUnchecked();

    process_ = other.process_;
    other.process_ = nullptr;

    applied_ = other.applied_;
    other.applied_ = false;

    target_ = other.target_;
    other.target_ = nullptr;

    data_ = std::move(other.data_);

    orig_ = std::move(other.orig_);

    return *this;
  }

  ~PatchRaw()
  {
    RemoveUnchecked();
  }

  bool IsApplied() const HADESMEM_DETAIL_NOEXCEPT
  {
    return applied_;
  }

  void Apply()
  {
    if (applied_)
    {
      return;
    }

    if (detached_)
    {
      HADESMEM_DETAIL_ASSERT(false);
      return;
    }

    SuspendedProcess const suspended_process{process_->GetId()};

    detail::VerifyPatchThreads(process_->GetId(), target_, data_.size());

    orig_ = ReadVector<std::uint8_t>(*process_, target_, data_.size());

    WriteVector(*process_, target_, data_);

    FlushInstructionCache(*process_, target_, data_.size());

    applied_ = true;
  }

  void Remove()
  {
    if (!applied_)
    {
      return;
    }

    SuspendedProcess const suspended_process{process_->GetId()};

    detail::VerifyPatchThreads(process_->GetId(), target_, data_.size());

    WriteVector(*process_, target_, orig_);

    FlushInstructionCache(*process_, target_, orig_.size());

    applied_ = false;
  }

  void Detach()
  {
    applied_ = false;

    detached_ = true;
  }

private:
  void RemoveUnchecked() HADESMEM_DETAIL_NOEXCEPT
  {
    try
    {
      Remove();
    }
    catch (...)
    {
      // WARNING: Patch may not be removed if Remove fails.
      HADESMEM_DETAIL_TRACE_A(
        boost::current_exception_diagnostic_information().c_str());
      HADESMEM_DETAIL_ASSERT(false);

      process_ = nullptr;
      applied_ = false;

      target_ = nullptr;
      data_.clear();
      orig_.clear();
    }
  }

  Process const* process_;
  bool applied_{false};
  bool detached_{false};
  PVOID target_;
  std::vector<BYTE> data_;
  std::vector<std::uint8_t> orig_;
};

class PatchDetour
{
public:
  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchDetour(Process const& process,
                       TargetFuncT target,
                       DetourFuncT detour)
    : process_{&process},
      target_{detail::AliasCast<void*>(target)},
      detour_{detail::AliasCast<void*>(detour)}
  {
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<TargetFuncT>::value ||
                                  std::is_pointer<TargetFuncT>::value);
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<DetourFuncT>::value ||
                                  std::is_pointer<DetourFuncT>::value);
  }

  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchDetour(Process&& process,
                       TargetFuncT target,
                       DetourFuncT detour) = delete;

  PatchDetour(PatchDetour const& other) = delete;

  PatchDetour& operator=(PatchDetour const& other) = delete;

  PatchDetour(PatchDetour&& other)
    : process_{other.process_},
      applied_{other.applied_},
      target_{other.target_},
      detour_{other.detour_},
      trampoline_{std::move(other.trampoline_)},
      orig_(std::move(other.orig_)),
      trampolines_(std::move(other.trampolines_)),
      ref_count_{other.ref_count_.load()}
  {
    other.process_ = nullptr;
    other.applied_ = false;
    other.target_ = nullptr;
    other.detour_ = nullptr;
  }

  PatchDetour& operator=(PatchDetour&& other)
  {
    RemoveUnchecked();

    process_ = other.process_;
    other.process_ = nullptr;

    applied_ = other.applied_;
    other.applied_ = false;

    target_ = other.target_;
    other.target_ = nullptr;

    detour_ = other.detour_;
    other.detour_ = nullptr;

    trampoline_ = std::move(other.trampoline_);

    orig_ = std::move(other.orig_);

    trampolines_ = std::move(other.trampolines_);

    ref_count_ = other.ref_count_.load();

    return *this;
  }

  virtual ~PatchDetour()
  {
    RemoveUnchecked();
  }

  bool IsApplied() const HADESMEM_DETAIL_NOEXCEPT
  {
    return applied_;
  }

  void Apply()
  {
    if (applied_)
    {
      return;
    }

    if (detached_)
    {
      HADESMEM_DETAIL_ASSERT(false);
      return;
    }

    // Reset the trampolines here because we don't do it in remove, otherwise
    // there's a potential race condition where we want to unhook and unload
    // safely, so we unhook the function, then try waiting on our ref count to
    // become zero, but we haven't actually called the trampoline yet, so we end
    // up jumping to the memory we just free'd!
    trampoline_ = nullptr;
    trampolines_.clear();

    SuspendedProcess const suspended_process{process_->GetId()};

    std::uint32_t const kMaxInstructionLen = 15;
    std::uint32_t const kTrampSize = kMaxInstructionLen * 3;

    trampoline_ = std::make_unique<Allocator>(*process_, kTrampSize);
    auto tramp_cur = static_cast<std::uint8_t*>(trampoline_->GetBase());

    HADESMEM_DETAIL_TRACE_FORMAT_A("Target = %p, Detour = %p, Trampoline = %p.",
                                   target_,
                                   detour_,
                                   trampoline_->GetBase());

    auto const buffer =
      ReadVector<std::uint8_t>(*process_, target_, kTrampSize);

    ud_t ud_obj;
    ud_init(&ud_obj);
    ud_set_input_buffer(&ud_obj, buffer.data(), buffer.size());
    ud_set_syntax(&ud_obj, UD_SYN_INTEL);
    ud_set_pc(&ud_obj, reinterpret_cast<std::uint64_t>(target_));
#if defined(HADESMEM_DETAIL_ARCH_X64)
    ud_set_mode(&ud_obj, 64);
#elif defined(HADESMEM_DETAIL_ARCH_X86)
    ud_set_mode(&ud_obj, 32);
#else
#error "[HadesMem] Unsupported architecture."
#endif

    std::size_t const patch_size = GetPatchSize();

    std::uint32_t instr_size = 0;
    do
    {
      std::uint32_t const len = ud_disassemble(&ud_obj);
      if (len == 0)
      {
        HADESMEM_DETAIL_THROW_EXCEPTION(Error{}
                                        << ErrorString{"Disassembly failed."});
      }

#if !defined(HADESMEM_NO_TRACE)
      char const* const asm_str = ud_insn_asm(&ud_obj);
      char const* const asm_bytes_str = ud_insn_hex(&ud_obj);
      HADESMEM_DETAIL_TRACE_FORMAT_A(
        "%s. [%s].",
        (asm_str ? asm_str : "Invalid."),
        (asm_bytes_str ? asm_bytes_str : "Invalid."));
#endif

      ud_operand_t const* const op = ud_insn_opr(&ud_obj, 0);
      bool is_jimm = op && op->type == UD_OP_JIMM;
      // Handle JMP QWORD PTR [RIP+Rel32]. Necessary for hook chain support.
      bool is_jmem = op && op->type == UD_OP_MEM && op->base == UD_R_RIP &&
                     op->index == UD_NONE && op->scale == 0 && op->size == 0x40;
      if ((ud_obj.mnemonic == UD_Ijmp || ud_obj.mnemonic == UD_Icall) && op &&
          (is_jimm || is_jmem))
      {
        std::uint16_t const size = is_jimm ? op->size : op->offset;
        HADESMEM_DETAIL_TRACE_FORMAT_A("Operand/offset size is %hu.", size);
        std::int64_t const insn_target = [&]() -> std::int64_t
        {
          switch (size)
          {
          case sizeof(std::int8_t) * CHAR_BIT:
            return op->lval.sbyte;
          case sizeof(std::int16_t) * CHAR_BIT:
            return op->lval.sword;
          case sizeof(std::int32_t) * CHAR_BIT:
            return op->lval.sdword;
          case sizeof(std::int64_t) * CHAR_BIT:
            return op->lval.sqword;
          default:
            HADESMEM_DETAIL_ASSERT(false);
            HADESMEM_DETAIL_THROW_EXCEPTION(
              Error{} << ErrorString{"Unknown instruction size."});
          }
        }();

        auto const resolve_rel =
          [](std::uint64_t base, std::int64_t target, std::uint32_t insn_len)
        {
          return reinterpret_cast<std::uint8_t*>(
                   static_cast<std::uintptr_t>(base)) +
                 target + insn_len;
        };

        std::uint64_t const insn_base = ud_insn_off(&ud_obj);
        std::uint32_t const insn_len = ud_insn_len(&ud_obj);

        auto const resolved_target =
          resolve_rel(insn_base, insn_target, insn_len);
        void* const jump_target =
          is_jimm ? resolved_target : Read<void*>(*process_, resolved_target);
        HADESMEM_DETAIL_TRACE_FORMAT_A("Jump/call target = %p.", jump_target);
        if (ud_obj.mnemonic == UD_Ijmp)
        {
          HADESMEM_DETAIL_TRACE_A("Writing resolved jump.");
          tramp_cur += WriteJump(tramp_cur, jump_target, true);
        }
        else
        {
          HADESMEM_DETAIL_ASSERT(ud_obj.mnemonic == UD_Icall);
          HADESMEM_DETAIL_TRACE_A("Writing resolved call.");
          tramp_cur += WriteCall(tramp_cur, jump_target);
        }
      }
      else
      {
        std::uint8_t const* const raw = ud_insn_ptr(&ud_obj);
        Write(*process_, tramp_cur, raw, raw + len);
        tramp_cur += len;
      }

      instr_size += len;
    } while (instr_size < patch_size);

    HADESMEM_DETAIL_TRACE_A("Writing jump back to original code.");

    tramp_cur += WriteJump(
      tramp_cur, static_cast<std::uint8_t*>(target_) + instr_size, true);

    FlushInstructionCache(
      *process_, trampoline_->GetBase(), trampoline_->GetSize());

    orig_ = ReadVector<std::uint8_t>(*process_, target_, patch_size);

    detail::VerifyPatchThreads(process_->GetId(), target_, orig_.size());

    WritePatch();

    FlushInstructionCache(*process_, target_, instr_size);

    applied_ = true;
  }

  void Remove()
  {
    if (!applied_)
    {
      return;
    }

    SuspendedProcess const suspended_process{process_->GetId()};

    detail::VerifyPatchThreads(process_->GetId(), target_, orig_.size());
    detail::VerifyPatchThreads(
      process_->GetId(), trampoline_->GetBase(), trampoline_->GetSize());

    RemovePatch();

    // Don't free trampolines here. Do it in Apply/destructor. See comments in
    // Apply for the rationale.

    applied_ = false;
  }

  void Detach()
  {
    applied_ = false;

    detached_ = true;
  }

  PVOID GetTrampoline() const HADESMEM_DETAIL_NOEXCEPT
  {
    return trampoline_->GetBase();
  }

  template <typename FuncT> FuncT GetTrampoline() const HADESMEM_DETAIL_NOEXCEPT
  {
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<FuncT>::value ||
                                  std::is_pointer<FuncT>::value);
    return hadesmem::detail::AliasCastUnchecked<FuncT>(trampoline_->GetBase());
  }

  // Ref count is user-managed and only here for convenience purposes.
  std::atomic<std::uint32_t>& GetRefCount()
  {
    return ref_count_;
  }

  std::atomic<std::uint32_t> const& GetRefCount() const
  {
    return ref_count_;
  }

  bool CanHookChain() const
  {
    return CanHookChainImpl();
  }

protected:
  virtual std::size_t GetPatchSize() const
  {
    bool detour_near = IsNear(target_, detour_);
    HADESMEM_DETAIL_TRACE_A(detour_near ? "Detour near." : "Detour far.");
    return detour_near ? kJmpSize32 : kJmpSize64;
  }

  virtual void WritePatch()
  {
    HADESMEM_DETAIL_TRACE_A("Writing jump to detour.");

    WriteJump(target_, detour_, false);
  }

  virtual void RemovePatch()
  {
    HADESMEM_DETAIL_TRACE_A("Restoring original bytes.");

    WriteVector(*process_, target_, orig_);
  }

  virtual bool CanHookChainImpl() const
  {
    return true;
  }

  void RemoveUnchecked() HADESMEM_DETAIL_NOEXCEPT
  {
    try
    {
      Remove();
    }
    catch (...)
    {
      // WARNING: Patch may not be removed if Remove fails.
      HADESMEM_DETAIL_TRACE_A(
        boost::current_exception_diagnostic_information().c_str());
      HADESMEM_DETAIL_ASSERT(false);

      process_ = nullptr;
      applied_ = false;

      target_ = nullptr;
      detour_ = nullptr;
      trampoline_.reset();
      orig_.clear();
      trampolines_.clear();
    }
  }

  // Inspired by EasyHook.
  std::unique_ptr<Allocator> AllocatePageNear(PVOID address)
  {
    SYSTEM_INFO sys_info{};
    GetSystemInfo(&sys_info);
    DWORD const page_size = sys_info.dwPageSize;

#if defined(HADESMEM_DETAIL_ARCH_X64)
    std::intptr_t const search_beg = (std::max)(
      reinterpret_cast<std::intptr_t>(address) - 0x7FFFFF00LL,
      reinterpret_cast<std::intptr_t>(sys_info.lpMinimumApplicationAddress));
    std::intptr_t const search_end = (std::min)(
      reinterpret_cast<std::intptr_t>(address) + 0x7FFFFF00LL,
      reinterpret_cast<std::intptr_t>(sys_info.lpMaximumApplicationAddress));

    std::unique_ptr<Allocator> trampoline;

    auto const allocate_tramp =
      [](Process const& process, PVOID addr, SIZE_T size)
        -> std::unique_ptr<Allocator>
    {
      auto const new_addr = detail::TryAlloc(process, size, addr);
      return new_addr
               ? std::make_unique<Allocator>(process, size, new_addr, true)
               : std::unique_ptr<Allocator>();
    };

    // Do two separate passes when looking for trampolines, ensuring to scan
    // forwards first. This is because there is a bug in Steam's overlay (last
    // checked and confirmed in SteamOverlayRender64.dll v2.50.25.37) where
    // negative displacements are not correctly sign-extended when cast to
    // 64-bits, resulting in a crash when they attempt to resolve the jump.

    // .text:0000000180082956                 cmp     al, 0FFh
    // .text:0000000180082958                 jnz     short loc_180082971
    // .text:000000018008295A                 cmp     byte ptr [r13+1], 25h
    // .text:000000018008295F                 jnz     short loc_180082971
    // ; Notice how the displacement is not being sign extended.
    // .text:0000000180082961                 mov     eax, [r13+2]
    // .text:0000000180082965                 lea     rcx, [rax+r13]
    // .text:0000000180082969                 mov     r13, [rcx+6]

    for (std::intptr_t base = reinterpret_cast<std::intptr_t>(address),
                       index = 0;
         base + index < search_end && !trampoline;
         index += page_size)
    {
      trampoline = allocate_tramp(
        *process_, reinterpret_cast<void*>(base + index), page_size);
    }

    if (!trampoline)
    {
      HADESMEM_DETAIL_TRACE_A(
        "WARNING! Failed to find a viable trampoline "
        "page in forward scan, falling back to backward scan. This may cause "
        "incompatibilty with some other overlays.");
    }

    for (std::intptr_t base = reinterpret_cast<std::intptr_t>(address),
                       index = 0;
         base - index > search_beg && !trampoline;
         index += page_size)
    {
      trampoline = allocate_tramp(
        *process_, reinterpret_cast<void*>(base - index), page_size);
    }

    if (!trampoline)
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{"Failed to find trampoline memory block."});
    }

    return trampoline;
#elif defined(HADESMEM_DETAIL_ARCH_X86)
    (void)address;
    return std::make_unique<Allocator>(*process_, page_size);
#else
#error "[HadesMem] Unsupported architecture."
#endif
  }

  bool IsNear(void* address, void* target) const HADESMEM_DETAIL_NOEXCEPT
  {
#if defined(HADESMEM_DETAIL_ARCH_X64)
    auto const rel = reinterpret_cast<std::intptr_t>(target) -
                     reinterpret_cast<std::intptr_t>(address) - 5;
    return rel > (std::numeric_limits<std::uint32_t>::min)() &&
           rel < (std::numeric_limits<std::uint32_t>::max)();
#elif defined(HADESMEM_DETAIL_ARCH_X86)
    (void)address;
    (void)target;
    return true;
#else
#error "[HadesMem] Unsupported architecture."
#endif
  }

  std::vector<std::uint8_t> GenJmp32(void* address, void* target) const
  {
    std::vector<std::uint8_t> buf = {0xE9, 0xEB, 0xBE, 0xAD, 0xDE};
    auto const dst_len = sizeof(std::uint32_t);
    auto const op_len = 1;
    auto const disp = reinterpret_cast<std::uintptr_t>(target) -
                      reinterpret_cast<std::uintptr_t>(address) - dst_len -
                      op_len;
    *reinterpret_cast<std::uint32_t*>(&buf[op_len]) =
      static_cast<std::uint32_t>(disp);
    return buf;
  }

  std::vector<std::uint8_t> GenCall32(void* address, void* target) const
  {
    std::vector<std::uint8_t> buf = {0xE8, 0xEB, 0xBE, 0xAD, 0xDE};
    auto const dst_len = sizeof(std::uint32_t);
    auto const op_len = 1;
    auto const disp = reinterpret_cast<std::uintptr_t>(target) -
                      reinterpret_cast<std::uintptr_t>(address) - dst_len -
                      op_len;
    *reinterpret_cast<std::uint32_t*>(&buf[op_len]) =
      static_cast<std::uint32_t>(disp);
    return buf;
  }

  std::vector<std::uint8_t> GenJmpTramp64(void* address, void* target) const
  {
    std::vector<std::uint8_t> buf = {0xFF, 0x25, 0xEF, 0xBE, 0xAD, 0xDE};
    auto const dst_len = sizeof(std::uint32_t);
    auto const op_len = 2;
    auto const disp = reinterpret_cast<std::uintptr_t>(target) -
                      reinterpret_cast<std::uintptr_t>(address) - dst_len -
                      op_len;
    *reinterpret_cast<std::uint32_t*>(&buf[op_len]) =
      static_cast<std::uint32_t>(disp);
    return buf;
  }

  std::vector<std::uint8_t> GenCallTramp64(void* address, void* target) const
  {
    std::vector<std::uint8_t> buf = {0xFF, 0x15, 0xEF, 0xBE, 0xAD, 0xDE};
    auto const dst_len = sizeof(std::uint32_t);
    auto const op_len = 2;
    auto const disp = reinterpret_cast<std::uintptr_t>(target) -
                      reinterpret_cast<std::uintptr_t>(address) - dst_len -
                      op_len;
    *reinterpret_cast<std::uint32_t*>(&buf[op_len]) =
      static_cast<std::uint32_t>(disp);
    return buf;
  }

  std::vector<std::uint8_t> GenPush32Ret(void* target) const
  {
    std::vector<std::uint8_t> buf = {// PUSH 0xDEADBEEF
                                     0x68,
                                     0xEF,
                                     0xBE,
                                     0xAD,
                                     0xDE,
                                     // RET
                                     0xC3};
    auto const op_len = 1;
    auto const target_low = static_cast<std::uint32_t>(
      reinterpret_cast<std::uintptr_t>(target) & 0xFFFFFFFF);
    *reinterpret_cast<std::uint32_t*>(&buf[op_len]) = target_low;
    return buf;
  }

  std::vector<std::uint8_t> GenPush64Ret(void* target) const
  {
    std::vector<std::uint8_t> buf = {// PUSH 0xDEADBEEF
                                     0x68,
                                     0xEF,
                                     0xBE,
                                     0xAD,
                                     0xDE,
                                     // MOV DWORD PTR [RSP+0x4], 0xDEADBEEF
                                     0xC7,
                                     0x44,
                                     0x24,
                                     0x04,
                                     0xEF,
                                     0xBE,
                                     0xAD,
                                     0xDE,
                                     // RET
                                     0xC3};
    auto const low_data_offs = 1;
    auto const high_data_offs = 9;
    auto const target_uint = reinterpret_cast<std::uint64_t>(target);
    auto const target_high =
      static_cast<std::uint32_t>((target_uint >> 32) & 0xFFFFFFFF);
    auto const target_low =
      static_cast<std::uint32_t>(target_uint & 0xFFFFFFFF);
    *reinterpret_cast<std::uint32_t*>(&buf[low_data_offs]) = target_low;
    *reinterpret_cast<std::uint32_t*>(&buf[high_data_offs]) = target_high;
    return buf;
  }

  std::size_t WriteJump(void* address, void* target, bool push_ret_fallback)
  {
    (void)push_ret_fallback;
    HADESMEM_DETAIL_TRACE_FORMAT_A(
      "Address = %p, Target = %p, Push Ret Fallback = %u.",
      address,
      target,
      static_cast<std::uint32_t>(push_ret_fallback));

    std::vector<std::uint8_t> jump_buf;

#if defined(HADESMEM_DETAIL_ARCH_X64)
    if (IsNear(address, target))
    {
      HADESMEM_DETAIL_TRACE_A("Using relative jump.");
      jump_buf = GenJmp32(address, target);
      HADESMEM_DETAIL_ASSERT(jump_buf.size() == kJmpSize32);
    }
    else
    {
      std::unique_ptr<Allocator> trampoline;
      try
      {
        trampoline = AllocatePageNear(address);
      }
      catch (std::exception const& /*e*/)
      {
        // Don't need to do anything, we'll fall back to PUSH/RET.
      }

      if (trampoline)
      {
        void* tramp_addr = trampoline->GetBase();

        HADESMEM_DETAIL_TRACE_FORMAT_A(
          "Using trampoline jump. Trampoline = %p.", tramp_addr);

        Write(*process_, tramp_addr, target);

        trampolines_.emplace_back(std::move(trampoline));

        jump_buf = GenJmpTramp64(address, tramp_addr);
        HADESMEM_DETAIL_ASSERT(jump_buf.size() == kJmpSize64);
      }
      else
      {
        if (!push_ret_fallback)
        {
          // We're out of options...
          HADESMEM_DETAIL_THROW_EXCEPTION(
            Error{} << ErrorString{"Unable to use a relative or trampoline "
                                   "jump, and push/ret fallback is disabled."});
        }

        HADESMEM_DETAIL_TRACE_A("Using push/ret 'jump'.");

        auto const target_high = static_cast<std::uint32_t>(
          (reinterpret_cast<std::uintptr_t>(target) >> 32) & 0xFFFFFFFF);
        if (target_high)
        {
          HADESMEM_DETAIL_TRACE_A("Push/ret 'jump' is big.");
          jump_buf = GenPush64Ret(target);
          HADESMEM_DETAIL_ASSERT(jump_buf.size() == kPushRetSize64);
        }
        else
        {
          HADESMEM_DETAIL_TRACE_A("Push/ret 'jump' is small.");
          jump_buf = GenPush32Ret(target);
          HADESMEM_DETAIL_ASSERT(jump_buf.size() == kPushRetSize32);
        }
      }
    }
#elif defined(HADESMEM_DETAIL_ARCH_X86)
    HADESMEM_DETAIL_TRACE_A("Using relative jump.");
    jump_buf = GenJmp32(address, target);
    HADESMEM_DETAIL_ASSERT(jump_buf.size() == kJmpSize32);
#else
#error "[HadesMem] Unsupported architecture."
#endif

    WriteVector(*process_, address, jump_buf);

    return jump_buf.size();
  }

  std::size_t WriteCall(void* address, void* target)
  {
    HADESMEM_DETAIL_TRACE_FORMAT_A(
      "Address = %p, Target = %p", address, target);

    std::vector<std::uint8_t> call_buf;

#if defined(HADESMEM_DETAIL_ARCH_X64)
    std::unique_ptr<Allocator> trampoline = AllocatePageNear(address);

    PVOID tramp_addr = trampoline->GetBase();

    HADESMEM_DETAIL_TRACE_FORMAT_A("Using trampoline call. Trampoline = %p.",
                                   tramp_addr);

    Write(*process_, tramp_addr, reinterpret_cast<std::uintptr_t>(target));

    trampolines_.emplace_back(std::move(trampoline));

    call_buf = GenCallTramp64(address, tramp_addr);
    HADESMEM_DETAIL_ASSERT(call_buf.size() == kCallSize64);
#elif defined(HADESMEM_DETAIL_ARCH_X86)
    HADESMEM_DETAIL_TRACE_A("Using relative call.");
    call_buf = GenCall32(address, target);
    HADESMEM_DETAIL_ASSERT(call_buf.size() == kCallSize32);
#else
#error "[HadesMem] Unsupported architecture."
#endif

    WriteVector(*process_, address, call_buf);

    return call_buf.size();
  }

  static std::size_t const kJmpSize32 = 5;
  static std::size_t const kCallSize32 = 5;
#if defined(HADESMEM_DETAIL_ARCH_X64)
  static std::size_t const kJmpSize64 = 6;
  static std::size_t const kCallSize64 = 6;
  static std::size_t const kPushRetSize64 = 14;
  static std::size_t const kPushRetSize32 = 6;
#elif defined(HADESMEM_DETAIL_ARCH_X86)
  static std::size_t const kJmpSize64 = kJmpSize32;
  static std::size_t const kCallSize64 = kCallSize32;
#else
#error "[HadesMem] Unsupported architecture."
#endif

  Process const* process_;
  bool applied_{false};
  bool detached_{false};
  PVOID target_;
  PVOID detour_;
  std::unique_ptr<Allocator> trampoline_;
  std::vector<BYTE> orig_;
  std::vector<std::unique_ptr<Allocator>> trampolines_;
  std::atomic<std::uint32_t> ref_count_;
};

class PatchVeh : public PatchDetour
{
public:
  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchVeh(Process const& process,
                    TargetFuncT target,
                    DetourFuncT detour)
    : PatchDetour{process, target, detour}
  {
    if (process.GetId() != ::GetCurrentProcessId())
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{
          "VEH based hooks on remote processes are currently unsupported."});
    }

    Initialize();

    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<TargetFuncT>::value ||
                                  std::is_pointer<TargetFuncT>::value);
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<DetourFuncT>::value ||
                                  std::is_pointer<DetourFuncT>::value);
  }

  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchVeh(Process&& process,
                    TargetFuncT target,
                    DetourFuncT detour) = delete;

  PatchVeh(PatchVeh const& other) = delete;

  PatchVeh& operator=(PatchVeh const& other) = delete;

  PatchVeh(PatchVeh&& other) : PatchDetour{std::move(other)}
  {
  }

  PatchVeh& operator=(PatchVeh&& other)
  {
    PatchDetour::operator=(std::move(other));
    return *this;
  }

protected:
  virtual std::size_t GetPatchSize() const override
  {
    HADESMEM_DETAIL_THROW_EXCEPTION(Error{} << ErrorString{"Unimplemented."});
  }

  virtual void WritePatch() override
  {
    HADESMEM_DETAIL_THROW_EXCEPTION(Error{} << ErrorString{"Unimplemented."});
  }

  virtual void RemovePatch() override
  {
    HADESMEM_DETAIL_THROW_EXCEPTION(Error{} << ErrorString{"Unimplemented."});
  }

  virtual bool CanHookChainImpl() const override
  {
    HADESMEM_DETAIL_THROW_EXCEPTION(Error{} << ErrorString{"Unimplemented."});
  }

  static void Initialize()
  {
    auto& initialized = GetInitialized();
    if (initialized)
    {
      return;
    }

    auto const veh_handle = ::AddVectoredExceptionHandler(1, &VectoredHandler);
    if (!veh_handle)
    {
      DWORD const last_error = ::GetLastError();
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{"AddVectoredExceptionHandler failed."}
                << ErrorCodeWinLast{last_error});
    }
    static detail::SmartRemoveVectoredExceptionHandler const remove_veh(
      veh_handle);

    initialized = true;
  }

  static LONG CALLBACK VectoredHandler(PEXCEPTION_POINTERS exception_pointers)
  {
    switch (exception_pointers->ExceptionRecord->ExceptionCode)
    {
    case EXCEPTION_BREAKPOINT:
      return HandleBreakpoint(exception_pointers);

    case EXCEPTION_SINGLE_STEP:
      return HandleSingleStep(exception_pointers);

    default:
      return EXCEPTION_CONTINUE_SEARCH;
    }
  }

  static LONG CALLBACK HandleBreakpoint(PEXCEPTION_POINTERS exception_pointers)
  {
    hadesmem::detail::AcquireSRWLock const lock(
      &GetSrwLock(), hadesmem::detail::SRWLockType::Shared);

    auto& veh_hooks = GetVehHooks();
    auto const iter =
      veh_hooks.find(exception_pointers->ExceptionRecord->ExceptionAddress);
    if (iter == std::end(veh_hooks))
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    PatchVeh* patch = iter->second;
#if defined(HADESMEM_DETAIL_ARCH_X64)
    exception_pointers->ContextRecord->Rip =
      reinterpret_cast<std::uintptr_t>(patch->detour_);
#elif defined(HADESMEM_DETAIL_ARCH_X86)
    exception_pointers->ContextRecord->Eip =
      reinterpret_cast<std::uintptr_t>(patch->detour_);
#else
#error "[HadesMem] Unsupported architecture."
#endif

    return EXCEPTION_CONTINUE_EXECUTION;
  }

  static LONG CALLBACK HandleSingleStep(PEXCEPTION_POINTERS exception_pointers)
  {
    hadesmem::detail::AcquireSRWLock const lock(
      &GetSrwLock(), hadesmem::detail::SRWLockType::Shared);

    auto& veh_hooks = GetVehHooks();
    auto const iter =
      veh_hooks.find(exception_pointers->ExceptionRecord->ExceptionAddress);
    if (iter == std::end(veh_hooks))
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    auto& dr_hooks = GetDrHooks();
    auto const dr_hook_iter = dr_hooks.find(::GetCurrentThreadId());
    if (dr_hook_iter == std::end(dr_hooks))
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    std::uintptr_t const dr_index = dr_hook_iter->second;
    if (!(exception_pointers->ContextRecord->Dr6 & (1ULL << dr_index)))
    {
      return EXCEPTION_CONTINUE_SEARCH;
    }

    // Reset status register
    exception_pointers->ContextRecord->Dr6 = 0;
    // Set resume flag
    exception_pointers->ContextRecord->EFlags |= (1ULL << 16);

    PatchVeh* patch = iter->second;
#if defined(HADESMEM_DETAIL_ARCH_X64)
    exception_pointers->ContextRecord->Rip =
      reinterpret_cast<std::uintptr_t>(patch->detour_);
#elif defined(HADESMEM_DETAIL_ARCH_X86)
    exception_pointers->ContextRecord->Eip =
      reinterpret_cast<std::uintptr_t>(patch->detour_);
#else
#error "[HadesMem] Unsupported architecture."
#endif

    return EXCEPTION_CONTINUE_EXECUTION;
  }

  static bool& GetInitialized()
  {
    static bool initialized = false;
    return initialized;
  }

  static std::map<void*, PatchVeh*>& GetVehHooks()
  {
    static std::map<void*, PatchVeh*> veh_hooks;
    return veh_hooks;
  }

  static std::map<DWORD, std::uintptr_t>& GetDrHooks()
  {
    static std::map<DWORD, std::uintptr_t> dr_hooks;
    return dr_hooks;
  }

  static SRWLOCK& GetSrwLock()
  {
    static SRWLOCK srw_lock = SRWLOCK_INIT;
    return srw_lock;
  }
};

class PatchInt3 : public PatchVeh
{
public:
  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchInt3(Process const& process,
                     TargetFuncT target,
                     DetourFuncT detour)
    : PatchVeh{process, target, detour}
  {
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<TargetFuncT>::value ||
                                  std::is_pointer<TargetFuncT>::value);
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<DetourFuncT>::value ||
                                  std::is_pointer<DetourFuncT>::value);
  }

  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchInt3(Process&& process,
                     TargetFuncT target,
                     DetourFuncT detour) = delete;

  PatchInt3(PatchInt3 const& other) = delete;

  PatchInt3& operator=(PatchInt3 const& other) = delete;

  PatchInt3(PatchInt3&& other) : PatchVeh{std::move(other)}
  {
  }

  PatchInt3& operator=(PatchInt3&& other)
  {
    PatchVeh::operator=(std::move(other));
    return *this;
  }

protected:
  virtual std::size_t GetPatchSize() const override
  {
    // 0xCC
    return 1;
  }

  virtual void WritePatch() override
  {
    auto& veh_hooks = GetVehHooks();

    {
      hadesmem::detail::AcquireSRWLock const lock(
        &GetSrwLock(), hadesmem::detail::SRWLockType::Exclusive);

      HADESMEM_DETAIL_ASSERT(veh_hooks.find(target_) == std::end(veh_hooks));
      veh_hooks[target_] = this;
    }

    auto const cleanup_hook = [&]()
    {
      veh_hooks.erase(target_);
    };
    auto scope_cleanup_hook = hadesmem::detail::MakeScopeWarden(cleanup_hook);

    HADESMEM_DETAIL_TRACE_A("Writing breakpoint.");

    std::vector<std::uint8_t> const buf = {0xCC};
    WriteVector(*process_, target_, buf);

    scope_cleanup_hook.Dismiss();
  }

  virtual void RemovePatch() override
  {
    HADESMEM_DETAIL_TRACE_A("Restoring original bytes.");

    WriteVector(*process_, target_, orig_);

    {
      hadesmem::detail::AcquireSRWLock const lock(
        &GetSrwLock(), hadesmem::detail::SRWLockType::Exclusive);

      auto& veh_hooks = GetVehHooks();
      veh_hooks.erase(target_);
    }
  }

  virtual bool CanHookChainImpl() const override
  {
    return false;
  }
};

// DANGER DANGER WILL ROBINSON
// This currently has some serious limitations. Notably:
//   Not even close to 'production' quality. Full of subtle bugs, gaps, etc.
//   Can only hook the 'current' thread.
//   Can only set one hook per thread.
//   No validation. e.g. Lets you orphan an existing hook by setting a new one.
//   Stomps over other things which may be using the debug registers.
//   Stomps over other types of VEH hooks (e.g. will stomp over an INT3 hook
//     on the same address).
//   Not handling TID reuse or invalidation.
//   Other bad things. Seriously, my head hurts from thinking of all the corner
//     cases.
class PatchDr : public PatchVeh
{
public:
  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchDr(Process const& process,
                   TargetFuncT target,
                   DetourFuncT detour)
    : PatchVeh{process, target, detour}
  {
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<TargetFuncT>::value ||
                                  std::is_pointer<TargetFuncT>::value);
    HADESMEM_DETAIL_STATIC_ASSERT(detail::IsFunction<DetourFuncT>::value ||
                                  std::is_pointer<DetourFuncT>::value);
  }

  template <typename TargetFuncT, typename DetourFuncT>
  explicit PatchDr(Process&& process,
                   TargetFuncT target,
                   DetourFuncT detour) = delete;

  PatchDr(PatchDr const& other) = delete;

  PatchDr& operator=(PatchDr const& other) = delete;

  PatchDr(PatchDr&& other) : PatchVeh{std::move(other)}
  {
  }

  PatchDr& operator=(PatchDr&& other)
  {
    PatchVeh::operator=(std::move(other));
    return *this;
  }

protected:
  virtual std::size_t GetPatchSize() const override
  {
    // The patch size is actually zero, but we need to pretend that we've patch
    // something so we can generate the trampoline to jump over it.
    return 1;
  }

  virtual void WritePatch() override
  {
    hadesmem::detail::AcquireSRWLock const lock(
      &GetSrwLock(), hadesmem::detail::SRWLockType::Exclusive);

    auto& veh_hooks = GetVehHooks();

    HADESMEM_DETAIL_ASSERT(veh_hooks.find(target_) == std::end(veh_hooks));
    veh_hooks[target_] = this;

    auto const veh_cleanup_hook = [&]()
    {
      auto const veh_hooks_removed = veh_hooks.erase(target_);
      (void)veh_hooks_removed;
      HADESMEM_DETAIL_ASSERT(veh_hooks_removed);
    };
    auto scope_veh_cleanup_hook =
      hadesmem::detail::MakeScopeWarden(veh_cleanup_hook);

    HADESMEM_DETAIL_TRACE_A("Setting DR hook.");

    auto& dr_hooks = GetDrHooks();
    auto const thread_id = ::GetCurrentThreadId();
    HADESMEM_DETAIL_ASSERT(dr_hooks.find(thread_id) == std::end(dr_hooks));

    Thread const thread(thread_id);
    auto context = GetThreadContext(thread, CONTEXT_DEBUG_REGISTERS);

    std::uint32_t dr_index = static_cast<std::uint32_t>(-1);
    for (std::uint32_t i = 0; i < 4; ++i)
    {
      // Check whether the DR is available according to the control register
      bool const control_available = !(context.Dr7 & (1ULL << (i * 2)));
      // Check whether the DR is zero. Pobably not actually necessary, but
      // it's a nice additional sanity check. This may require a
      // user-controlable flag in future though if the code being hooked is
      // 'hostile'.
      bool const dr_available = !(&context.Dr0)[i];
      if (control_available && dr_available)
      {
        dr_index = i;
        break;
      }
    }

    if (dr_index == static_cast<std::uint32_t>(-1))
    {
      HADESMEM_DETAIL_THROW_EXCEPTION(
        Error{} << ErrorString{"No free debug registers."});
    }

    dr_hooks[ ::GetCurrentThreadId()] = dr_index;

    auto const dr_cleanup_hook = [&]()
    {
      auto const dr_hooks_removed = dr_hooks.erase(::GetCurrentThreadId());
      (void)dr_hooks_removed;
      HADESMEM_DETAIL_ASSERT(dr_hooks_removed);
    };
    auto scope_dr_cleanup_hook =
      hadesmem::detail::MakeScopeWarden(dr_cleanup_hook);

    (&context.Dr0)[dr_index] = reinterpret_cast<std::uintptr_t>(target_);
    // Set appropriate L0-L3 flag
    context.Dr7 |= static_cast<std::uintptr_t>(1ULL << (dr_index * 2));
    // Set appropriate RW0-RW3 field (Execution)
    std::uintptr_t break_type = 0;
    context.Dr7 |= (break_type << (16 + 4 * dr_index));
    // Set appropriate LEN0-LEN3 field (1 byte)
    std::uintptr_t break_len = 0;
    context.Dr7 |= (break_len << (18 + 4 * dr_index));
    // Set LE flag
    std::uintptr_t local_enable = 1 << 8;
    context.Dr7 |= local_enable;

    SetThreadContext(thread, context);

    scope_veh_cleanup_hook.Dismiss();
    scope_dr_cleanup_hook.Dismiss();
  }

  virtual void RemovePatch() override
  {
    hadesmem::detail::AcquireSRWLock const lock(
      &GetSrwLock(), hadesmem::detail::SRWLockType::Exclusive);

    HADESMEM_DETAIL_TRACE_A("Unsetting DR hook.");

    auto& dr_hooks = GetDrHooks();
    auto const thread_id = ::GetCurrentThreadId();
    auto const iter = dr_hooks.find(thread_id);
    HADESMEM_DETAIL_ASSERT(iter != std::end(dr_hooks));
    auto const dr_index = iter->second;

    Thread const thread(thread_id);
    auto context = GetThreadContext(thread, CONTEXT_DEBUG_REGISTERS);

    // Clear the appropriate DR
    *(&context.Dr0 + dr_index) = 0;
    // Clear appropriate L0-L3 flag
    context.Dr7 &= ~static_cast<std::uintptr_t>(1ULL << (dr_index * 2));

    SetThreadContext(thread, context);

    auto const dr_hooks_removed = dr_hooks.erase(thread_id);
    (void)dr_hooks_removed;
    HADESMEM_DETAIL_ASSERT(dr_hooks_removed);

    auto& veh_hooks = GetVehHooks();
    auto const veh_hooks_removed = veh_hooks.erase(target_);
    (void)veh_hooks_removed;
    HADESMEM_DETAIL_ASSERT(veh_hooks_removed);
  }

  virtual bool CanHookChainImpl() const override
  {
    return false;
  }
};
}

/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "FastForward"

#include "fast_forward.h"

#include "log.h"
#include "task.h"

using namespace rr;
using namespace std;

struct InstructionBuf {
  SupportedArch arch;
  uint8_t code_buf[32];
  int code_buf_len;
};

static InstructionBuf read_instruction(Task* t, remote_ptr<uint8_t> ip) {
  InstructionBuf result;
  result.arch = t->arch();
  result.code_buf_len =
      (int)t->read_bytes_fallible(ip, sizeof(result.code_buf), result.code_buf);
  return result;
}

struct DecodedInstruction {
  int operand_size;
  int length;
  bool modifies_flags;
};

static bool decode_x86_string_instruction(const InstructionBuf& code,
                                          DecodedInstruction* decoded) {
  bool found_operand_prefix = false;
  bool found_REP_prefix = false;
  bool found_REXW_prefix = false;

  decoded->modifies_flags = false;

  int i;
  bool done = false;
  for (i = 0; i < code.code_buf_len; ++i) {
    switch (code.code_buf[i]) {
      case 0x66:
        found_operand_prefix = true;
        break;
      case 0x48:
        if (code.arch == x86_64) {
          found_REXW_prefix = true;
          break;
        }
        return false;
      case 0xF2:
      case 0xF3:
        found_REP_prefix = true;
        break;
      case 0xA4: // MOVSB
      case 0xA5: // MOVSW
      case 0xAA: // STOSB
      case 0xAB: // STOSW
      case 0xAC: // LODSB
      case 0xAD: // LODSW
        done = true;
        break;
      case 0xA6: // CMPSB
      case 0xA7: // CMPSW
      case 0xAE: // SCASB
      case 0xAF: // SCASW
        decoded->modifies_flags = true;
        done = true;
        break;
      default:
        return false;
    }
    if (done) {
      break;
    }
  }

  if (!found_REP_prefix) {
    return false;
  }

  decoded->length = i + 1;
  if (code.code_buf[i] & 1) {
    decoded->operand_size =
        found_REXW_prefix ? 8 : (found_operand_prefix ? 2 : 4);
  } else {
    decoded->operand_size = 1;
  }
  return true;
}

static bool mem_intersect(remote_ptr<void> a1, int s1, remote_ptr<void> a2,
                          int s2) {
  assert(a1 + s1 > a1);
  assert(a2 + s2 > a2);
  return max(a1, a2) < min(a1 + s1, a2 + s2);
}

static void bound_iterations_for_watchpoint(Task* t, remote_ptr<void> reg,
                                            const DecodedInstruction& decoded,
                                            const WatchConfig& watch,
                                            uintptr_t* iterations) {
  // Compute how many iterations it will take before we hit the watchpoint.
  // 0 means the first iteration will hit the watchpoint.
  int size = decoded.operand_size;
  int direction = t->regs().df_flag() ? -1 : 1;

  if (mem_intersect(reg, size, watch.addr, watch.num_bytes)) {
    *iterations = 0;
    return;
  }

  // Number of iterations we can perform without triggering the watchpoint
  uintptr_t steps;
  if (direction > 0) {
    if (watch.addr < reg) {
      // We're assuming wraparound can't happpen!
      return;
    }
    // We'll hit the first byte of the watchpoint moving forward.
    steps = (watch.addr - reg) / size;
  } else {
    if (watch.addr > reg) {
      // We're assuming wraparound can't happpen!
      return;
    }
    // We'll hit the last byte of the watchpoint moving backward.
    steps = (reg - (watch.addr + watch.num_bytes)) / size + 1;
  }

  *iterations = min(*iterations, steps);
}

void fast_forward_through_instruction(Task* t, const Registers** states) {
  remote_ptr<uint8_t> ip = t->ip();

  t->resume_execution(RESUME_SINGLESTEP, RESUME_WAIT);
  ASSERT(t, t->pending_sig() == SIGTRAP);

  if (t->ip() != ip) {
    return;
  }
  if (t->vm()->get_breakpoint_type_at_addr(ip) != TRAP_NONE) {
    // breakpoint must have fired
    return;
  }
  if (t->debug_status() & DS_WATCHPOINT_ANY) {
    // watchpoint fired
    return;
  }
  for (size_t i = 0; states[i]; ++i) {
    if (states[i]->matches(t->regs())) {
      return;
    }
  }
  if (t->arch() != x86 && t->arch() != x86_64) {
    return;
  }

  InstructionBuf instruction_buf = read_instruction(t, ip);
  DecodedInstruction decoded;
  if (!decode_x86_string_instruction(instruction_buf, &decoded)) {
    return;
  }

  Registers extra_state_to_avoid;
  vector<const Registers*> states_copy;

  while (true) {
    // This string instruction should execute until CX reaches 0 and
    // we move to the next instruction, or we hit one of the states in
    // |states|, or the ZF flag changes so that the REP stops, or we hit
    // a watchpoint. (We can't hit a breakpoint during the loop since we
    // already verified there isn't one set here.)

    // We'll compute an upper bound on the number of string instruction
    // iterations to execute, and set a watchpoint on the memory location
    // accessed through DI in the iteration we want to stop at. We'll also
    // set a breakpoint after the string instruction to catch cases where it
    // ends due to a ZF change.
    // Keep in mind that it's possible that states in |states| might
    // belong to multiple independent loops of this string instruction, with
    // registers reset in between the loops.

    uintptr_t cur_cx = t->regs().cx();
    if (cur_cx == 0) {
      // This instruction will be skipped entirely.
      return;
    }

    // Don't execute the last iteration of the string instruction. That
    // simplifies code below that tries to emulate the register effects
    // of singlestepping to predict if the next singlestep would result in a
    // mark_vector state.
    uintptr_t iterations = cur_cx - 1;

    // Bound |iterations| to ensure we stop before reaching any |states|.
    for (size_t i = 0; states[i]; ++i) {
      auto state = states[i];
      if (state->ip() == ip) {
        uintptr_t dest_cx = state->cx();
        if (dest_cx == 0) {
          // This state represents entering the string instruction with CX==0,
          // so we can't reach this state in the current loop.
          continue;
        }
        if (dest_cx >= cur_cx) {
          // This can't be reached in the current loop.
          continue;
        }
        iterations = min(iterations, cur_cx - dest_cx - 1);
      } else if (state->ip() == ip + decoded.length) {
        uintptr_t dest_cx = state->cx();
        if (dest_cx >= cur_cx) {
          // This can't be reached in the current loop.
          continue;
        }
        iterations = min(iterations, cur_cx - dest_cx - 1);
      }
    }

    // To stop before the ZF changes and we exit the loop, we don't bound
    // the iterations here. Instead we run the loop, observe the ZF change,
    // and then rerun the loop with the loop-exit state added to the |states|
    // list. See below.

    // A code watchpoint would already be hit if we're going to hit it.
    // Check for data watchpoints that we might hit when reading/writing
    // memory.
    // Make conservative assumptions about the watchpoint type and assume
    // every string instruction uses SI and DI. Applying unnecessary bounds
    // here will only result in a few more singlesteps.
    for (auto& watch : t->vm()->all_watchpoints()) {
      bound_iterations_for_watchpoint(t, t->regs().si(), decoded, watch,
                                      &iterations);
      bound_iterations_for_watchpoint(t, t->regs().di(), decoded, watch,
                                      &iterations);
    }

    if (iterations == 0) {
      return;
    }

    LOG(debug) << "x86-string fast-forward: " << iterations
               << " iterations required";

    Registers r = t->regs();

    int direction = t->regs().df_flag() ? -1 : 1;
    // Figure out the address to set a watchpoint at. This address must
    // be accessed at or before the last iteration we want to perform.
    // We have to account for a CPU quirk: Intel CPUs may coalesce iterations
    // to write up to 64 bytes at a time (observed for "rep stosb" on Ivy
    // Bridge). Assume 128 bytes to be safe.
    static const int BYTES_COALESCED = 128;
    uintptr_t watch_offset = decoded.operand_size * (iterations - 1);
    if (watch_offset > BYTES_COALESCED) {
      watch_offset -= BYTES_COALESCED;
      t->vm()->save_watchpoints();
      t->vm()->remove_all_watchpoints();
      remote_ptr<void> watch_di = t->regs().di() + direction * watch_offset;
      LOG(debug) << "Set x86-string fast-forward watchpoint at " << watch_di;
      bool ok = t->vm()->add_watchpoint(watch_di, 1, WATCH_READWRITE);
      ASSERT(t, ok) << "Can't even handle one watchpoint???";
      ok = t->vm()->add_breakpoint(ip + decoded.length, TRAP_BKPT_INTERNAL);
      ASSERT(t, ok) << "Failed to add breakpoint";

      t->resume_execution(RESUME_CONT, RESUME_WAIT);
      ASSERT(t, t->pending_sig() == SIGTRAP);
      auto debug_status = t->consume_debug_status();
      if (!(debug_status & DS_WATCHPOINT_ANY)) {
        // watchpoint didn't fire. We must have exited the loop early and
        // hit the breakpoint. IP will be after the breakpoint instruction.
        ASSERT(t, t->ip() == ip + decoded.length + 1 && decoded.modifies_flags);
        // Undo the execution of the breakpoint instruction.
        Registers tmp = t->regs();
        tmp.set_ip(ip + decoded.length);
        t->set_regs(tmp);
      }

      t->vm()->remove_breakpoint(ip + decoded.length, TRAP_BKPT_INTERNAL);
      t->vm()->restore_watchpoints();

      iterations -= cur_cx - t->regs().cx();
    }

    LOG(debug) << "x86-string fast-forward: " << iterations
               << " iterations to go";

    // Singlestep through the remaining iterations.
    while (iterations > 0 && t->ip() == ip) {
      t->resume_execution(RESUME_SINGLESTEP, RESUME_WAIT);
      ASSERT(t, t->pending_sig() == SIGTRAP);
      auto debug_status = t->consume_debug_status();
      // No watchpoints should have fired. If we exited the loop, we should
      // still not have triggered any EXEC watchpoints since we haven't
      // executed any instructions outside the loop.
      ASSERT(t, !(debug_status & DS_WATCHPOINT_ANY));
      --iterations;
    }

    if (t->ip() != ip) {
      // We exited the loop early due to flags being modified.
      ASSERT(t, t->ip() == ip + decoded.length && decoded.modifies_flags);
      // String instructions that modify flags don't have non-register side
      // effects, so we can reset registers to effectively unwind the loop.
      // Then we try rerunning the loop again, adding this state as one to
      // avoid stepping into. We shouldn't need to do this more than once!
      ASSERT(t, states_copy.empty());
      for (size_t i = 0; states[i]; ++i) {
        states_copy.push_back(states[i]);
      }
      extra_state_to_avoid = t->regs();
      states_copy.push_back(&extra_state_to_avoid);
      states_copy.push_back(nullptr);
      states = states_copy.data();
      t->set_regs(r);
    } else {
      LOG(debug) << "x86-string fast-forward done";
      break;
    }
  }
}

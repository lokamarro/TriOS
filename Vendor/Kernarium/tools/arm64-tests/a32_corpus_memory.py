#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The memory suites of the AArch32 instruction corpus.

A load or store case names the address it wants; the planner solves the base
(and, for a register offset, chooses the index register) so the instruction's
effective address is exactly that, whatever the offset and shift. The
addresses probe what a memory system can get wrong: aligned and misaligned
accesses, accesses that cross a 4 KiB and a 16 KiB line, the last bytes of the
data region, the first byte of the read-only region, unmapped memory and
address zero.

- memory: single-register loads and stores and the multiple-register forms,
  at addresses an AArch32 process may use on any host, and the unaligned
  accesses a single-register form permits; then LDM, STM, PUSH and POP of
  two, six, seven and the most registers under a condition that fails and
  passes, within a page and across one;
- unaligned: the multi-word forms at unaligned addresses, which qemu-arm
  rejects with an alignment fault and an arm64 Linux kernel fixes up for a
  compat task, so only hardware records settle them (decision D3);
- exclusive: LDREX/STREX sequences, with CLREX, with a lone STREX and across
  addresses;
- exclusive-spill: the same pairs with a register spill (STR or STRD to the
  stack) between the load and the store, which hardware's monitor survives;
- positions: single-register and doubleword loads and stores, VLDR, VSTR, TBB
  and TBH that fault after other instructions, some in a block a branch
  reaches: unmapped, crossing from the read-only region into unmapped memory,
  and a literal below the code region; the unmapped and literal ones also
  under a condition that fails and passes. Then LDM, STM, PUSH and POP,
  unmapped and crossing from unmapped memory into the data region, and an
  A32 STM that stores the PC, unmapped and where it stores, within a page
  and across one, each also under a condition that fails and passes;
- positions-unaligned: the same at a misaligned address, which only hardware
  records settle.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re

from a32_corpus_plan import (  # noqa: E402
    FLAGS, ISA_LETTER, Case, Context, candidate_words, corpus, fields_of, get_field, pattern,
    set_field, usable, word_of,
)

MASK = 0xFFFFFFFF
D = corpus.DATA_BASE
READ_ONLY = corpus.READ_ONLY_BASE
INDEX_VALUES = (0x10, 4, 0x80000000, 0xFFFFFFF0, 1, 0x20, 0x7FFFFFFF, 3)
SIZED = re.compile(r"(LDR|STR|LDA|STL)(EX)?(SB|SH|B|H|D)?T?\Z")
MULTIPLE = re.compile(r"(LDM|STM)(DA|DB|IA|IB)?\Z|(PUSH|POP)\Z")
SIZE_OF = {None: 4, "B": 1, "H": 2, "D": 8, "SB": 1, "SH": 2}
RECIPES = ("edges", "pattern", "edges5", "pattern1", "edges3", "edges8")


def mnemonic(name: str) -> str:
    return name.split("_")[0]


def access_size(name: str):
    match = SIZED.fullmatch(mnemonic(name))
    return SIZE_OF[match.group(3)] if match else None


def multiple_mode(name: str) -> str:
    """IA, IB, DA or DB: where the transfer lies relative to the base."""
    text = mnemonic(name)
    if text == "PUSH":
        return "DB"
    if text == "POP":
        return "IA"
    return text[3:] or "IA"


def shift_value(value: int, kind: str, amount: int, carry: int) -> int:
    kind = kind.removeprefix("SRType_")
    value &= MASK
    if kind == "LSL":
        return value << amount & MASK
    if kind == "LSR":
        return value >> amount if amount < 32 else 0
    if kind == "ASR":
        signed = value - (1 << 32) if value >> 31 else value
        return signed >> min(amount, 31) & MASK
    if kind == "ROR":
        amount %= 32
        return (value >> amount | value << (32 - amount)) & MASK
    return (carry << 31 | value >> 1) & MASK  # RRX


def register_form(operands: dict) -> bool:
    return operands.get("register_form", "m" in operands and "imm32" not in operands)


def solve(operands: dict, address: int, apsr: int, choice: int):
    """Register assignments that make a single access's effective address
    `address`, or None when the instruction cannot be aimed (PC or a clashing
    index register)."""
    n = operands["n"]
    if n == 15:
        return None
    assignments = {}
    if register_form(operands):
        m = operands["m"]
        if m == 15 or m == n:
            return None
        value = INDEX_VALUES[choice % len(INDEX_VALUES)]
        assignments[f"r{m}"] = value
        offset = shift_value(value, operands.get("shift_t", "SRType_LSL"),
                             operands.get("shift_n", 0), apsr >> 29 & 1)
    else:
        offset = operands.get("imm32", 0) & MASK  # LDA, STL and the exclusives have none
    signed = offset if operands.get("add", True) else -offset
    index = operands.get("index", not operands.get("postindex", False))
    assignments[f"r{n}"] = (address - signed if index else address) & MASK
    return assignments


def solve_multiple(operands: dict, mode: str, start: int):
    """The base register value for a transfer whose lowest address is `start`."""
    count = bin(operands["registers"]).count("1")
    base = {"IA": start, "IB": start - 4, "DA": start + 4 * count - 4, "DB": start + 4 * count}[mode]
    return {f"r{operands.get('n', 13)}": base & MASK}


def single_scenarios(size: int):
    return {
        "a": D + 0x1040, "x4": D + 0x2000 - size // 2, "x16": D + 0x4000 - size // 2,
        "last": D + 0x8000 - size, "ro": READ_ONLY, "unmapped": 0x60000000, "zero": 0,
        "wrap": 0xFFFFFFFC, "u1": D + 0x1041, "u2": D + 0x1042, "u3": D + 0x1043,
    }


def multiple_scenarios(count: int):
    half = 4 * (count // 2)
    return {
        "a": D + 0x1040, "x4": D + 0x2000 - half, "x16": D + 0x4000 - half,
        "last": D + 0x8000 - 4 * count, "ro": READ_ONLY, "unmapped": 0x60000000, "zero": 0,
        "u1": D + 0x1041, "u2": D + 0x1042, "u3": D + 0x1043,
    }


def memory_forms(context: Context, level: str):
    for form in context.model.forms:
        if (form.encoding is None or form.level != level or form.availability != "implemented"
                or form.check or form.nop or "exclusive" in form.traits or "prefetch" in form.traits
                or not form.traits & {"loads", "stores"} or "branch" in form.traits):
            continue
        if MULTIPLE.fullmatch(mnemonic(form.name)) or access_size(form.name) is not None:
            yield form


def is_wide(form_name: str) -> bool:
    return MULTIPLE.fullmatch(mnemonic(form_name)) is not None or access_size(form_name) == 8


def pick_words(context, form, count=6):
    words = []
    for word in candidate_words(context, form, 8):
        found = usable(context, form, word)
        if found is not None and found[0] not in [w for w, _ in words]:
            words.append(found)
        if len(words) == count:
            break
    return words


def single_case(context, suite, form, index, word, operands, scenario, address, number):
    isa = form.encoding.isa
    suffix = "a" if isa == "A32" else "t"
    recipe = f"{RECIPES[(index + number) % len(RECIPES)]}-{suffix}"
    flags = FLAGS[pattern(number) >> 8 & 3]
    if "n" not in operands or operands["n"] == 15:  # literal: the address is the program's own
        return Case(suite, f"{form.name}/{index}.lit", form.name, ISA_LETTER[isa],
                    word_of(isa, word), 0, recipe, {"apsr": flags})
    aim = solve(operands, address, flags, number)
    if aim is None:
        return None
    return Case(suite, f"{form.name}/{index}.{scenario}", form.name, ISA_LETTER[isa],
                word_of(isa, word), 0, recipe, {"apsr": flags, **aim})


def multiple_case(context, suite, form, index, word, operands, scenario, start, number):
    isa = form.encoding.isa
    suffix = "a" if isa == "A32" else "t"
    recipe = f"{RECIPES[(index + number) % len(RECIPES)]}-{suffix}"
    flags = FLAGS[pattern(number) >> 8 & 3]
    aim = solve_multiple(operands, multiple_mode(form.name), start)
    return Case(suite, f"{form.name}/{index}.{scenario}", form.name, ISA_LETTER[isa],
                word_of(isa, word), 0, recipe, {"apsr": flags, **aim})


def build(context: Context, level: str, suite: str, ordinary: bool) -> list[Case]:
    """ordinary: the memory suite; otherwise the unaligned multi-word forms."""
    cases = []
    used = set()
    for form_index, form in enumerate(memory_forms(context, level)):
        wide = is_wide(form.name)
        multiple = MULTIPLE.fullmatch(mnemonic(form.name)) is not None
        size = access_size(form.name)
        names = (["a", "x4", "x16", "last", "ro", "unmapped", "zero", "wrap"] if ordinary else [])
        if multiple:
            names = [name for name in names if name != "wrap"]
        if ordinary and not wide and size >= 2:
            names += ["u1"] + (["u2", "u3"] if size == 4 else [])
        if not ordinary:
            if not wide:
                continue
            names = ["u1", "u2", "u3"]
        for index, (word, operands) in enumerate(pick_words(context, form)):
            for step in range(2 if ordinary else 1):
                number = len(cases) + form_index
                scenario = names[(index * 2 + step + form_index) % len(names)]
                if step == 0 and ordinary:
                    scenario = "a" if index % 2 == 0 else scenario
                if multiple:
                    count = bin(operands["registers"]).count("1")
                    start = multiple_scenarios(count)[scenario]
                    case = multiple_case(context, suite, form, index, word, operands, scenario,
                                         start, number)
                else:
                    address = single_scenarios(size)[scenario]
                    case = single_case(context, suite, form, index, word, operands, scenario,
                                       address, number)
                if case is not None and case.name not in used:
                    used.add(case.name)
                    cases.append(case)
    return cases


def encode_list(form, word: int, registers: int) -> int:
    """The word with its register list set to `registers` (bit 14 LR, bit 15 PC), in whichever
    fields the encoding keeps them."""
    fields = fields_of(form)
    width = fields["register_list"][1]
    word = set_field(word, *fields["register_list"], registers & ((1 << width) - 1))
    if width < 16:
        for name, bit in (("M", 14), ("P", 15)):
            if name in fields:
                word = set_field(word, *fields[name], registers >> bit & 1)
    return word


def block_lists(form, base: int) -> list[int]:
    """Register lists that leave out the base, of two, six and seven registers and of all the
    encoding takes besides the SP and the PC: the transfer keeps six of them in its first operand
    word and the rest in one more."""
    width = fields_of(form)["register_list"][1]
    names = set(fields_of(form))
    low = range(8) if width == 8 else range(13)
    usable_registers = [number for number in low if number != base]
    if width == 16 or "M" in names:
        usable_registers.append(14)
    lists = []
    for count in (2, 6, 7, len(usable_registers)):
        if count <= len(usable_registers):
            mask = sum(1 << number for number in usable_registers[:count])
            if mask not in lists:
                lists.append(mask)
    return lists


def block_condition_cases(context: Context, suite: str) -> list[Case]:
    """LDM, STM, PUSH and POP under EQ (in A32) or IT EQ (in Thumb), whose condition fails and
    passes, with lists of two, six, seven and the most registers, within a page and, for six and
    the most, across one, with and without writeback where the encoding chooses it."""
    cases = []
    blocks = [form for form in memory_forms(context, "base") if MULTIPLE.fullmatch(mnemonic(form.name))]
    for form_index, form in enumerate(blocks):
        isa = form.encoding.isa
        state = ISA_LETTER[isa]
        suffix = "a" if state == "A" else "t"
        found = block_word(context, form, False)
        if found is None:
            continue
        fields = fields_of(form)
        base = found[1].get("n", 13)
        for list_index, registers in enumerate(block_lists(form, base)):
            word = encode_list(form, found[0], registers)
            if "W" in fields:
                word = set_field(word, *fields["W"], 1 - list_index % 2 if list_index < 3 else 0)
            if state == "A":
                word = set_field(word, *fields["cond"], 14)
            checked = usable(context, form, word)
            if checked is None:
                continue
            word, operands = checked
            count = bin(operands["registers"]).count("1")
            wback = "w" if operands.get("wback", "n" not in operands) else ""
            half = 4 * (count // 2)
            scenarios = {"a": D + 0x1040}
            if list_index % 2 == 1:
                scenarios["x16"] = D + 0x4000 - half
            lead = LEADS[state][0][0]
            if state == "A":
                body = word_of(isa, set_field(word, *fields["cond"], 0))
            else:
                body = (0xBF08,) + word_of(isa, word)
            for number, (scenario, start) in enumerate(scenarios.items()):
                flags = FLAGS[pattern(len(cases) + 11) >> 8 & 3]
                aim = solve_multiple(operands, multiple_mode(form.name), start)
                for passes in (False, True):
                    apsr = (0x40000000 if passes else 0) | flags & 0xB0000000
                    cases.append(Case(
                        suite, f"{form.name}/c{count}{wback}.{scenario}-eq{'p' if passes else 'f'}", form.name,
                        state, lead + body, len(lead) if state == "A" else 2,
                        f"{RECIPES[(form_index + list_index + number) % len(RECIPES)]}-{suffix}",
                        {"apsr": apsr, **aim}))
    return cases


def suite_memory(context: Context) -> list[Case]:
    return build(context, "base", "memory", True) + block_condition_cases(context, "memory")


def suite_unaligned(context: Context) -> list[Case]:
    return build(context, "base", "unaligned", False)


def suite_prefetch(context: Context) -> list[Case]:
    """PLD, PLDW and PLI never fault, whatever the address names."""
    cases = []
    used = set()
    names = ["a", "unmapped", "zero", "wrap", "ro", "x4"]
    forms = [form for form in context.model.forms
             if form.encoding is not None and form.level == "base" and "prefetch" in form.traits
             and form.availability == "implemented"]
    for form_index, form in enumerate(forms):
        for index, (word, operands) in enumerate(pick_words(context, form, 4)):
            for step in range(2):
                scenario = names[(index + 2 * step + form_index) % len(names)]
                case = single_case(context, "prefetch", form, index, word, operands, scenario,
                                   single_scenarios(1)[scenario], len(cases) + form_index)
                if case is not None and case.name not in used:
                    used.add(case.name)
                    cases.append(case)
    return cases


# Exclusives -----------------------------------------------------------------------------------------------

EXCLUSIVE_SIZES = ("", "B", "H", "D")
CLREX = {"A": [0xF57FF01F], "T": [0xF3BF, 0x8F2F]}
# A register spill between the pair, as a compiler leaves one when it runs out
# of registers: STR r8 and STRD r8, r9 to [sp, #8], a granule none of the
# exclusive addresses is in. Hardware keeps the monitor across them. The
# program spills once before the pair as well, which an engine whose TLB the
# spill's page would miss (the gadgets') has filled by then, so that the
# spill between the pair costs no host exclusive monitor. They are a suite of
# their own: the lanes that interrupt the guest throughout measure the bare
# window of the pair, and a longer one only adds losses to them.
SPILLS = {"A": {"spill": [0xE58D8008], "spill_pair": [0xE1CD80F8]},
          "T": {"spill": [0xF8CD, 0x8008], "spill_pair": [0xE9CD, 0x8902]}}


def exclusive_word(context, name: str, **registers):
    """A decodable word of the form with the named register fields set."""
    form = context.model.by_name[name]
    fields = fields_of(form)
    for word in candidate_words(context, form, 12):
        found = usable(context, form, word, True)
        if found is None:
            continue
        word = found[0]
        for field, value in registers.items():
            if field in fields:
                word = set_field(word, *fields[field], value)
        again = usable(context, form, word, True)
        if again is not None:
            return again
    return None


def program_units(word: int, isa: str) -> list[int]:
    """One instruction as program units: its word in A32, its halfwords in Thumb."""
    if isa == "A32":
        return [word]
    return [word >> 16] if isa == "T16" else [word >> 16, word & 0xFFFF]


def exclusive_cases(context: Context, suite: str, load_stem: str, store_stem: str,
                    spill: bool = False) -> list[Case]:
    """The pairs, or with `spill` the pairs with a spill between them."""
    cases = []
    for state, suffix in (("A", "A1"), ("T", "T1")):
        letter = "a" if state == "A" else "t"
        for size_name in EXCLUSIVE_SIZES:
            size = {"": 4, "B": 1, "H": 2, "D": 8}[size_name]
            load_name, store_name = f"{load_stem}{size_name}_{suffix}", f"{store_stem}{size_name}_{suffix}"
            wide = size_name == "D"
            pair = {"Rt2": 3} if wide else {}  # Rt2 is SBO in the byte and halfword forms
            loads = exclusive_word(context, load_name, Rn=4, Rt=2 if wide else 1, **pair)
            stores = exclusive_word(context, store_name, Rn=4, Rd=5, Rt=6, **({"Rt2": 7} if wide else {}))
            isa = context.model.by_name[load_name].encoding.isa
            ldrex, strex = program_units(loads[0], isa), program_units(stores[0], isa)
            if state == "A":
                ldrex, strex = [set_field(ldrex[0], 28, 4, 14)], [set_field(strex[0], 28, 4, 14)]
            clrex = CLREX[state]

            def aimed(address, choice):
                if "imm32" in loads[1]:
                    return solve({**loads[1], "n": 4}, address, 0, choice)
                return {"r4": address}

            shapes = ({name: [spilled, ldrex, spilled, strex] for name, spilled in SPILLS[state].items()}
                      if spill else
                      {"pair": [ldrex, strex], "lone": [strex], "clrex": [ldrex, clrex, strex],
                       "twice": [ldrex, ldrex, strex]})
            for index, (scenario, address) in enumerate((("a", D + 0x1040), ("x4", D + 0x2000 - size),
                                                          ("last", D + 0x8000 - size))):
                aim = aimed(address, index)
                for shape, parts in shapes.items():
                    units = [unit for part in parts for unit in part]
                    target = [i for i, part in enumerate(parts) if part is strex][0]
                    recipe = f"{('edges', 'pattern')[index % 2]}-{letter}"
                    cases.append(Case(suite, f"{store_name}/{shape}.{scenario}", store_name, state,
                                      tuple(units), target, recipe, {**aim, "r5": 9}))
            if spill:
                continue
            # A load that faults: unmapped, or misaligned (an alignment fault, which
            # no kernel fixes up for an exclusive).
            faulting = {"unmapped": 0x60000000, "zero": 0}
            if size >= 2:
                faulting.update(u1=D + 0x1041, u2=D + 0x1042)
            if size == 8:
                faulting.update(u4=D + 0x1044)
            if size == 4:
                faulting.update(u3=D + 0x1043)
            for scenario, address in faulting.items():
                cases.append(Case(suite, f"{load_name}/load.{scenario}", load_name, state,
                                  tuple(ldrex), 0, f"edges-{letter}", aimed(address, 0)))
    return cases


def suite_exclusive(context: Context) -> list[Case]:
    """LDREX/STREX and the acquire/release LDAEX/STLEX, which are base too."""
    return (exclusive_cases(context, "exclusive", "LDREX", "STREX")
            + exclusive_cases(context, "exclusive", "LDAEX", "STLEX"))


def suite_exclusive_spill(context: Context) -> list[Case]:
    """The exclusive pairs with a spill between the load and the store."""
    return (exclusive_cases(context, "exclusive-spill", "LDREX", "STREX", True)
            + exclusive_cases(context, "exclusive-spill", "LDAEX", "STLEX", True))


# Positions -------------------------------------------------------------------------------------------------

# The instructions ahead of the faulting one: NOPs of both Thumb lengths, and a
# branch to the next instruction, which starts a block an engine links to.
LEADS = {
    "A": (((0xE320F000,),),
          ((0xE320F000,), (0xE320F000,), (0xE320F000,)),
          ((0xEAFFFFFF,), (0xE320F000,), (0xE320F000,))),
    "T": (((0xBF00,),),
          ((0xF3AF, 0x8000), (0xBF00,), (0xF3AF, 0x8000)),
          ((0xE7FF,), (0xF3AF, 0x8000), (0xBF00,))),
}
VFP_TRANSFER = re.compile(r"V(LDR|STR)(_l)?_[AT]1_([DSH])\Z")
RO_END = READ_ONLY + corpus.READ_ONLY_SIZE


def position_forms(context: Context):
    """(form, bytes, alignment): the transfers whose operand names their place in the block."""
    singles = {form.name for form in memory_forms(context, "base")
               if not MULTIPLE.fullmatch(mnemonic(form.name))}
    for form in context.model.forms:
        if form.encoding is None or form.availability != "implemented" or form.check or form.nop:
            continue
        vfp = VFP_TRANSFER.fullmatch(form.name)
        if vfp:
            size = {"D": 8, "S": 4, "H": 2}[vfp.group(3)]
            yield form, size, min(size, 4)
        elif form.name in ("TBB_T1", "TBH_T1"):
            yield form, 1 if form.name == "TBB_T1" else 2, 1
        elif form.name in singles:
            size = access_size(form.name)
            ordered = mnemonic(form.name).startswith(("LDA", "STL"))
            yield form, size, size if ordered else 4 if size == 8 else 1


def position_scenarios(name: str, size: int, alignment: int, literal: bool) -> dict[str, int]:
    if literal:
        return {"below": 0}
    scenarios = {"unmapped": 0x60000000}
    loads = name.startswith(("LDR", "VLDR", "TB"))
    if loads and size > 1 and (RO_END - size // 2) % alignment == 0:
        scenarios["cross"] = RO_END - size // 2
    if alignment > 1:
        scenarios["u1"] = D + 0x1041
    return scenarios


def position_word(context, form, literal: bool):
    """A word of the form to aim, or for a literal one whose address is below the code region."""
    fields = fields_of(form)
    for word, operands in pick_words(context, form, 12):
        if literal:
            if "U" in fields:  # subtract a large, aligned offset: the top immediate field's largest
                immediates = [name for name in fields if name.startswith("imm")]
                word = set_field(word, *fields["U"], 0)
                for index, name in enumerate(immediates):
                    word = set_field(word, *fields[name], (1 << fields[name][1]) - 1 if index == 0 else 0)
                found = usable(context, form, word)
                if found is not None and not found[1].get("add", True) and found[1].get("imm32", 0) >= 0x40:
                    return found
        elif operands.get("n") not in (None, 15) and operands.get("m") not in (13, 15) \
                and operands.get("m", -1) != operands["n"]:
            return word, operands
    return None


def position_cases(context: Context, suite: str, misaligned: bool) -> list[Case]:
    """The cases of the positions suites: the misaligned ones, which only hardware settles, or the rest."""
    cases = []
    conditional_cases = []
    for form_index, (form, size, alignment) in enumerate(position_forms(context)):
        isa = form.encoding.isa
        state = ISA_LETTER[isa]
        suffix = "a" if state == "A" else "t"
        literal = form.name.split("_")[1] == "l"
        found = position_word(context, form, literal)
        if found is None:
            continue
        word, operands = found
        for number, (scenario, address) in enumerate(
                position_scenarios(form.name, size, alignment, literal).items()):
            # The A72 the hardware records come from has no half-precision VLDR or VSTR.
            if (scenario == "u1") != misaligned or misaligned and form.name.endswith("_H"):
                continue
            leads = LEADS[state][(form_index + number) % len(LEADS[state])]
            flags = FLAGS[pattern(len(cases)) >> 8 & 3]
            initial = {"apsr": flags}
            if form.name.startswith("TB"):
                initial.update({f"r{operands['n']}": address, f"r{operands['m']}": 0})
            elif not literal:
                aim = solve(operands, address, flags, len(cases))
                if aim is None:
                    continue
                initial.update(aim)
            linked = leads[0] in ((0xEAFFFFFF,), (0xE7FF,))
            program = tuple(unit for lead in leads for unit in lead) + word_of(isa, word)
            cases.append(Case(suite, f"{form.name}/{scenario}.{len(leads)}", form.name, state,
                              program, len(leads), f"{RECIPES[(form_index + number) % len(RECIPES)]}-{suffix}",
                              initial, straight=not linked,
                              fault_retired=len(leads) if linked else None))
            if misaligned or scenario not in ("unmapped", "below"):
                continue
            # The same transfer under EQ (in A32) or IT EQ (in Thumb), whose condition fails, so
            # nothing faults, and passes. A half-precision VLDR or VSTR is UNPREDICTABLE under a
            # condition.
            fields = fields_of(form)
            if state == "A" and "cond" not in fields or form.name.endswith("_H"):
                continue
            lead = LEADS[state][0][0]
            if state == "A":
                body = word_of(isa, set_field(word, *fields["cond"], 0))
            else:
                body = (0xBF08,) + word_of(isa, word)
            for passes in (False, True):
                apsr = (0x40000000 if passes else 0) | flags & 0xB0000000
                conditional = dict(initial, apsr=apsr)
                if not form.name.startswith("TB") and not literal:
                    aim = solve(operands, address, apsr, len(conditional_cases))
                    if aim is None:
                        continue
                    conditional.update(aim)
                conditional_cases.append(Case(
                    suite, f"{form.name}/{scenario}-eq{'p' if passes else 'f'}", form.name, state, lead + body,
                    len(lead) if state == "A" else 2,
                    f"{RECIPES[(form_index + number + 1) % len(RECIPES)]}-{suffix}", conditional))
    return cases + conditional_cases + block_position_cases(context, suite, misaligned)


def block_word(context, form, store_pc: bool):
    """A word of a block transfer of two or more registers that leaves out its base, with the PC in
    its list for an STM that stores it."""
    fields = fields_of(form)
    for word, operands in pick_words(context, form, 12):
        if store_pc:
            word = set_field(word, *fields["register_list"], operands["registers"] | 0x8000)
            found = usable(context, form, word)
            if found is None:
                continue
            word, operands = found
        registers = operands["registers"]
        if bin(registers).count("1") >= 2 and not registers >> operands.get("n", 13) & 1:
            return word, operands
    return None


def block_position_cases(context: Context, suite: str, misaligned: bool) -> list[Case]:
    """LDM, STM, PUSH and POP that fault after other instructions: unmapped, crossing from unmapped
    memory into the data region, which the library runs and where nothing is transferred before the
    fault, and misaligned; an A32 STM that stores the PC, which the library runs, also where it
    stores, within a page and across one; all but the misaligned ones also under a condition that
    fails and passes."""
    cases = []
    conditional_cases = []
    blocks = [form for form in memory_forms(context, "base") if MULTIPLE.fullmatch(mnemonic(form.name))]
    variants = [(form, False) for form in blocks] + [
        (form, True) for form in blocks
        if form.encoding.isa == "A32" and mnemonic(form.name).startswith("STM")]
    for form_index, (form, store_pc) in enumerate(variants):
        isa = form.encoding.isa
        state = ISA_LETTER[isa]
        suffix = "a" if state == "A" else "t"
        found = block_word(context, form, store_pc)
        if found is None:
            continue
        word, operands = found
        count = bin(operands["registers"]).count("1")
        prefix = "pc-" if store_pc else ""
        if misaligned:
            scenarios = {"u1": D + 0x1041}
        else:
            scenarios = {"unmapped": 0x60000000, "into": D - 4 * (count // 2)}
            if store_pc:
                scenarios = {"a": D + 0x1040, "x16": D + 0x4000 - 4 * (count // 2), "unmapped": 0x60000000}
        for number, (scenario, start) in enumerate(scenarios.items()):
            leads = LEADS[state][(form_index + number) % len(LEADS[state])]
            flags = FLAGS[pattern(len(cases) + 7) >> 8 & 3]
            initial = {"apsr": flags, **solve_multiple(operands, multiple_mode(form.name), start)}
            linked = leads[0] in ((0xEAFFFFFF,), (0xE7FF,))
            program = tuple(unit for lead in leads for unit in lead) + word_of(isa, word)
            cases.append(Case(suite, f"{form.name}/{prefix}{scenario}.{len(leads)}", form.name, state,
                              program, len(leads), f"{RECIPES[(form_index + number) % len(RECIPES)]}-{suffix}",
                              initial, straight=not linked,
                              fault_retired=len(leads) if linked and scenario != "a" else None))
            if misaligned:
                continue
            fields = fields_of(form)
            lead = LEADS[state][0][0]
            if state == "A":
                body = word_of(isa, set_field(word, *fields["cond"], 0))
            else:
                body = (0xBF08,) + word_of(isa, word)
            for passes in (False, True):
                apsr = (0x40000000 if passes else 0) | flags & 0xB0000000
                conditional_cases.append(Case(
                    suite, f"{form.name}/{prefix}{scenario}-eq{'p' if passes else 'f'}", form.name, state,
                    lead + body, len(lead) if state == "A" else 2,
                    f"{RECIPES[(form_index + number + 1) % len(RECIPES)]}-{suffix}",
                    dict(initial, apsr=apsr)))
    return cases + conditional_cases


def suite_positions(context: Context) -> list[Case]:
    return position_cases(context, "positions", False)


def suite_positions_unaligned(context: Context) -> list[Case]:
    return position_cases(context, "positions-unaligned", True)


SUITES = {"memory": suite_memory, "unaligned": suite_unaligned, "exclusive": suite_exclusive,
          "exclusive-spill": suite_exclusive_spill, "prefetch": suite_prefetch,
          "positions": suite_positions, "positions-unaligned": suite_positions_unaligned}

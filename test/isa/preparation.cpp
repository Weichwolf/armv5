// SPDX-License-Identifier: GPL-3.0-only
#include "support/isa.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

struct Case {
  std::uint32_t word;
  std::string_view name;
};

void data_processing() {
  constexpr unsigned instruction_mask = 0x0fffffff;
  constexpr unsigned rotated_immediate = 0xa80;
  constexpr std::array names{"AND",
                             "EOR",
                             "SUB",
                             "RSB",
                             "ADD",
                             "ADC",
                             "SBC",
                             "RSC",
                             "TST",
                             "TEQ",
                             "CMP",
                             "CMN",
                             "ORR",
                             "MOV",
                             "BIC",
                             "MVN"};
  for (unsigned opcode = 0; opcode < names.size(); ++opcode) {
    const bool test = opcode >= 8 && opcode <= 11;
    const unsigned source = opcode == 13 || opcode == 15 ? 0 : 1;
    const unsigned destination = test ? 0 : 2;
    for (unsigned condition = 0; condition < 15; ++condition) {
      for (const bool flags : {false, true}) {
        if (test && !flags) { continue; }
        // Immediate rotate, immediate shift (including RRX), register shift.
        for (const unsigned operand :
             {0x003U, 0x183U, 0x063U, 0x863U, 0x213U, 0x233U, 0x253U, 0x273U}) {
          const auto word = fixture::data(opcode, flags, destination, source, false, operand);
          fixture::decoded((word & instruction_mask) | (condition << fixture::condition_shift),
                           names[opcode]);
        }
        fixture::decoded(fixture::data(opcode, flags, destination, source, true, rotated_immediate),
                         names[opcode]);
      }
    }
  }
}

void extension_collisions() {

  // Distinct operations share the major data-processing and transfer spaces.
  // Literal fixtures pin reviewed encodings independently of the field builder.
  constexpr std::array cases{Case{.word = 0xe0000291, .name = "MUL"},
                             Case{.word = 0xe0203291, .name = "MLA"},
                             Case{.word = 0xe0810392, .name = "UMULL"},
                             Case{.word = 0xe0a10392, .name = "UMLAL"},
                             Case{.word = 0xe0c10392, .name = "SMULL"},
                             Case{.word = 0xe0e10392, .name = "SMLAL"},
                             Case{.word = 0xe1020091, .name = "SWP"},
                             Case{.word = 0xe1420091, .name = "SWPB"},
                             Case{.word = 0xe10f0000, .name = "MRS"},
                             Case{.word = 0xe14f0000, .name = "MRS"},
                             Case{.word = 0xe128f001, .name = "MSR"},
                             Case{.word = 0xe368f0ff, .name = "MSR"},
                             Case{.word = 0xe12fff11, .name = "BX"},
                             Case{.word = 0xe12fff21, .name = "BXJ"},
                             Case{.word = 0xe12fff31, .name = "BLX register"},
                             Case{.word = 0xe16f0f11, .name = "CLZ"},
                             Case{.word = 0xe1020051, .name = "QADD"},
                             Case{.word = 0xe1220051, .name = "QSUB"},
                             Case{.word = 0xe1420051, .name = "QDADD"},
                             Case{.word = 0xe1620051, .name = "QDSUB"},
                             Case{.word = 0xe1200070, .name = "BKPT"},
                             Case{.word = 0xe1003281, .name = "SMLAxy"},
                             Case{.word = 0xe1203281, .name = "SMLAWy"},
                             Case{.word = 0xe12002a1, .name = "SMULWy"},
                             Case{.word = 0xe1410382, .name = "SMLALxy"},
                             Case{.word = 0xe1600281, .name = "SMULxy"},
                             Case{.word = 0xe1d100b2, .name = "LDRH"},
                             Case{.word = 0xe1c100b2, .name = "STRH"},
                             Case{.word = 0xe1d100d2, .name = "LDRSB"},
                             Case{.word = 0xe1d100f2, .name = "LDRSH"},
                             Case{.word = 0xe1c200d0, .name = "LDRD"},
                             Case{.word = 0xe1c200f0, .name = "STRD"},
                             Case{.word = 0xe19100b2, .name = "LDRH"},
                             Case{.word = 0xe18100b2, .name = "STRH"},
                             Case{.word = 0xe19100d2, .name = "LDRSB"},
                             Case{.word = 0xe19100f2, .name = "LDRSH"},
                             Case{.word = 0xe18200d3, .name = "LDRD"},
                             Case{.word = 0xe18200f3, .name = "STRD"},
                             Case{.word = 0xea000001, .name = "B"},
                             Case{.word = 0xeb000001, .name = "BL"},
                             Case{.word = 0xfa000001, .name = "BLX immediate"},
                             Case{.word = 0xfb000001, .name = "BLX immediate"},
                             Case{.word = 0xf5d1f004, .name = "PLD"},
                             Case{.word = 0xf7d1f002, .name = "PLD"},
                             Case{.word = 0xe8910005, .name = "LDM"},
                             Case{.word = 0xe8810005, .name = "STM"},
                             Case{.word = 0xef123456, .name = "SWI"},
                             Case{.word = 0xec410f02, .name = "MCRR"},
                             Case{.word = 0xec510f02, .name = "MRRC"},
                             Case{.word = 0xed910f00, .name = "LDC"},
                             Case{.word = 0xed810f00, .name = "STC"},
                             Case{.word = 0xfd910f00, .name = "LDC2"},
                             Case{.word = 0xfd810f00, .name = "STC2"},
                             Case{.word = 0xee010f02, .name = "CDP"},
                             Case{.word = 0xfe010f02, .name = "CDP2"},
                             Case{.word = 0xee110f10, .name = "MRC"},
                             Case{.word = 0xee010f10, .name = "MCR"},
                             Case{.word = 0xfe110f10, .name = "MRC2"},
                             Case{.word = 0xfe010f10, .name = "MCR2"}};
  for (const auto &[word, name] : cases) { fixture::decoded(word, name); }
  for (const bool load : {false, true}) {
    for (const bool byte : {false, true}) {
      const std::string_view load_name = byte ? "LDRB" : "LDR";
      const std::string_view store_name = byte ? "STRB" : "STR";
      fixture::decoded(fixture::transfer(load, byte, 2, 1), load ? load_name : store_name);
    }
  }
}

void rejected_encodings() {
  // ARMv6 media/exclusive/UMAAL and unconditional control must not masquerade as
  // ARMv5 ALU, transfer, multiply, or condition-never operations.
  constexpr std::array cases{Case{.word = 0xe6bf0f31, .name = "undefined media"},
                             Case{.word = 0xe1900f9f, .name = "swap/exclusive"},
                             Case{.word = 0xe0400291, .name = "undefined multiply"},
                             Case{.word = 0xf1080080, .name = "undefined unconditional"},
                             Case{.word = 0xf1010200, .name = "undefined unconditional"},
                             Case{.word = 0xf8900a00, .name = "undefined unconditional"},
                             Case{.word = 0xfc410f02, .name = "undefined coprocessor extension"},
                             Case{.word = 0xec010f00, .name = "undefined coprocessor extension"},
                             Case{.word = 0xe3000000, .name = "control/DSP encoding"},
                             Case{.word = 0xe10f0100, .name = "control/DSP encoding"},
                             Case{.word = 0xe0001291, .name = "MUL reserved field"},
                             Case{.word = 0x11200070, .name = "BKPT condition"},
                             Case{.word = 0xe1601281, .name = "SMULxy reserved field"},
                             Case{.word = 0xe12012a1, .name = "SMULWy reserved field"},
                             Case{.word = 0xe19101b2, .name = "register offset reserved field"},
                             Case{.word = 0xe0f100b2, .name = "post-index writeback"},
                             Case{.word = 0xf7d1f012, .name = "PLD register shift"},
                             Case{.word = 0xe7f000f0, .name = "undefined media"}};
  for (const auto &[word, reason] : cases) { fixture::rejected(word, reason); }

  fixture::Program prefix;
  constexpr std::uint32_t move_one = 0xe3a00001;
  constexpr std::uint32_t undefined = 0xe7f000f0;
  prefix.word(move_one); // MOV, followed by a malformed encoding.
  prefix.word(undefined);
  fixture::require(prefix.diagnostic().find("0x00001004") != std::string::npos,
                   "preparation must identify the rejected word after a valid prefix");
}

} // namespace

int main() {
  try {
    data_processing();
    extension_collisions();
    rejected_encodings();
    std::cout
        << "ARM preparation: encoding classes and rejection diagnostics passed; no execution\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

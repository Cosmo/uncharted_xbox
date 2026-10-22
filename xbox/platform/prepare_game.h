#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Makes the launcher's game files from the player's own dump, byte for byte what scripts/prepare.py,
// link_modules.py and content_profile.py write (run.py: boot-linked.bin and content.bin).
namespace UnchartedPrepare {
using Bytes=std::vector<uint8_t>;
// exe: the executable run.py prepares (eboot.bin, the collection menu and Uncharted 1, or
// big2-ps4_Shipping.elf / big3-ps4_Shipping.elf); the modules and param.sfo are the dump's.
struct Dump { Bytes exe,libc,fios,sfo; };
struct Files { Bytes boot,content; };
// The collection's executables, as run.py names them (u1, u2, u3).
inline constexpr const char* Executables[]={"eboot.bin","big2-ps4_Shipping.elf","big3-ps4_Shipping.elf"};
// Empty when param.sfo names Uncharted: The Nathan Drake Collection, else what is wrong, for the player.
std::string CheckTitle(const Bytes& sfo);
// Throws std::runtime_error with the reason when the dump cannot be used.
Files Prepare(const Dump& dump);
// Identifies the dump files that prepared files were made from.
uint64_t Fingerprint(const Dump& dump);
}

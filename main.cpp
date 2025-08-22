// Copyright (C) 2025 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include <CLI/CLI.hpp>
#include <llvm/Object/MachO.h>
#include <llvm/Object/MachOUniversal.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>


using namespace llvm;
using namespace object;

namespace {

// Struct to hold all command line arguments, returned by the parser.
struct CommandLineArgs
{
    std::string           binaryPath;
    std::set<std::string> excludedClasses;
    bool                  quietMode {false};
    bool                  dryRun {false};
    std::string           pattern;
    std::string           replacement;
};

// New function to parse command line arguments using CLI11.
// Returns an optional struct. If parsing fails or --help is used,
// it returns std::nullopt.
std::optional<CommandLineArgs> parseCommandLine(int argc, char** argv)
{
    auto     args = CommandLineArgs {};
    CLI::App app {"A tool to patch Objective-C metadata in Mach-O binaries."};

    // Positional argument for the file to patch.
    app.add_option("binary_to_patch", args.binaryPath, "The binary file to patch")
        ->required()
        ->check(CLI::ExistingFile);

    // Flags for quiet mode and dry run.
    app.add_flag("--quiet", args.quietMode, "Suppress output messages");
    app.add_flag("--dry-run", args.dryRun, "Perform a dry run without modifying the file");

    // Option to exclude classes, can be used multiple times.
    app.add_option("--exclude", args.excludedClasses, "List of class names to exclude from patching")
        ->type_name("CLASS");

    // Option for replacement mode. Takes two arguments: pattern and replacement.
    std::vector<std::string> replace_args;
    app.add_option("--replace", replace_args, "Replace a pattern with a replacement string")
        ->expected(2)
        ->type_name("PATTERN REPLACEMENT");

    // Custom validation logic after parsing.
    app.callback([&]() {
        if (!replace_args.empty()) {
            args.pattern     = replace_args[0];
            args.replacement = replace_args[1];

            if (args.pattern.empty()) {
                throw CLI::ValidationError("Error: replacement pattern cannot be empty.");
            }
            if (args.pattern.length() != args.replacement.length()) {
                throw CLI::ValidationError("Error: for binary safety, the replacement pattern and "
                                           "the "
                                           "replacement string must be the same length.");
            }
        }
    });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // exit() prints the error message and returns the appropriate exit code.
        // Returning nullopt signals the main function to exit.
        app.exit(e);
        return std::nullopt;
    }

    return args;
}

// Generates a random alphanumeric string of a given length.
std::string generateRandomString(size_t length)
{
    constexpr std::string_view charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ01234"
                                         "56789";
    static std::mt19937        generator(std::random_device {}());
    std::uniform_int_distribution<int> distribution(0, charset.length() - 1);
    std::string                        random_string;
    for (size_t i = 0; i < length; ++i) {
        random_string += charset[distribution(generator)];
    }
    return random_string;
}

// Converts a virtual address (VA) to a file offset relative to the start of the Mach-O slice.
std::optional<uint64_t> virtualAddressToFileOffset(const MachOObjectFile* Obj, uint64_t VA)
{
    for (const auto& LCI : Obj->load_commands()) {
        if (LCI.C.cmd == MachO::LC_SEGMENT_64) {
            const MachO::segment_command_64 Seg = Obj->getSegment64LoadCommand(LCI);
            if (VA >= Seg.vmaddr && VA < (Seg.vmaddr + Seg.vmsize)) {
                return (VA - Seg.vmaddr) + Seg.fileoff;
            }
        } else if (LCI.C.cmd == MachO::LC_SEGMENT) {
            const MachO::segment_command Seg = Obj->getSegmentLoadCommand(LCI);
            if (VA >= Seg.vmaddr && VA < (Seg.vmaddr + Seg.vmsize)) {
                return (VA - Seg.vmaddr) + Seg.fileoff;
            }
        }
    }
    return std::nullopt;
}

// Patches the __objc_classname section, skipping any names in the excluded list.
// Now accepts the CommandLineArgs struct.
void patchClassNameSection(const SectionRef&      Section,
                           const MachOObjectFile* MachOObj,
                           WritableMemoryBuffer&  WritableMB,
                           uint64_t               SliceOffset,
                           const CommandLineArgs& args)
{
    uint64_t SectionFileOffset = 0;
    if (MachOObj->is64Bit()) {
        const MachO::section_64 Sec = MachOObj->getSection64(Section.getRawDataRefImpl());
        SectionFileOffset           = Sec.offset;
    } else {
        const MachO::section Sec = MachOObj->getSection(Section.getRawDataRefImpl());
        SectionFileOffset        = Sec.offset;
    }

    Expected<StringRef> ContentsOrErr = Section.getContents();
    if (auto E = ContentsOrErr.takeError()) {
        consumeError(std::move(E));
        return;
    }
    StringRef   Contents = *ContentsOrErr;
    const char* Current  = Contents.begin();
    while (Current < Contents.end()) {
        StringRef Name(Current);
        if (Name.empty()) {
            Current++;
            continue;
        }

        if (args.excludedClasses.count(Name.str())) {
            if (!args.quietMode)
                outs() << "[CLASS] Skipping excluded class: " << Name.str() << "\n";
            Current += Name.size() + 1;
            continue;
        }

        uint64_t RealFileOffset = SliceOffset + SectionFileOffset + (Current - Contents.begin());
        bool     useReplaceMode = !args.pattern.empty();

        if (useReplaceMode) {
            std::string originalName = Name.str();
            std::string newName      = originalName;
            size_t      pos          = 0;
            bool        replaced     = false;
            while ((pos = newName.find(args.pattern, pos)) != std::string::npos) {
                newName.replace(pos, args.pattern.length(), args.replacement);
                pos += args.replacement.length();
                replaced = true;
            }

            if (replaced) {
                if (!args.quietMode) {
                    outs() << "[CLASS] Found: " << originalName << " at file offset "
                           << RealFileOffset << "\n"
                           << "  -> Replaced with: " << newName << "\n";
                }
                char* PatchLocation = WritableMB.getBufferStart() + RealFileOffset;
                memcpy(PatchLocation, newName.c_str(), originalName.length());
            }
        } else { // Randomization mode
            if (!args.quietMode)
                outs() << "[CLASS] Found: " << Name.str() << " at file offset " << RealFileOffset
                       << "\n";

            std::string RandomString  = generateRandomString(Name.size());
            char*       PatchLocation = WritableMB.getBufferStart() + RealFileOffset;
            memcpy(PatchLocation, RandomString.c_str(), RandomString.length());
            if (!args.quietMode)
                outs() << "  -> Replaced with: " << RandomString << "\n";
        }

        Current += Name.size() + 1;
    }
}

// Patches the names pointed to by the __objc_catlist section.
// Now accepts the CommandLineArgs struct.
void patchCategoryListSection(const SectionRef&      Section,
                              const MachOObjectFile* MachOObj,
                              const MemoryBuffer&    OriginalMB,
                              WritableMemoryBuffer&  WritableMB,
                              uint64_t               SliceOffset,
                              const CommandLineArgs& args)
{
    Expected<StringRef> ContentsOrErr = Section.getContents();
    if (auto E = ContentsOrErr.takeError()) {
        consumeError(std::move(E));
        return;
    }
    StringRef   Contents = *ContentsOrErr;
    const char* Data     = Contents.data();
    unsigned    PtrSize  = MachOObj->is64Bit() ? 8 : 4;

    for (unsigned i = 0; i + PtrSize <= Contents.size(); i += PtrSize) {
        uint64_t category_va         = (PtrSize == 8) ? *(const uint64_t*)(Data + i)
                                                      : *(const uint32_t*)(Data + i);
        auto     category_offset_opt = virtualAddressToFileOffset(MachOObj, category_va);
        if (!category_offset_opt)
            continue;

        const char* CategoryStructPtr
            = OriginalMB.getBufferStart() + SliceOffset + *category_offset_opt;
        uint64_t category_name_va = (PtrSize == 8) ? *(const uint64_t*)CategoryStructPtr
                                                   : *(const uint32_t*)CategoryStructPtr;

        auto name_offset_opt = virtualAddressToFileOffset(MachOObj, category_name_va);
        if (!name_offset_opt)
            continue;

        uint64_t  RealNameOffset = SliceOffset + *name_offset_opt;
        StringRef CategoryName(OriginalMB.getBufferStart() + RealNameOffset);
        if (CategoryName.empty())
            continue;

        bool useReplaceMode = !args.pattern.empty();

        if (useReplaceMode) {
            std::string originalName = CategoryName.str();
            std::string newName      = originalName;
            size_t      pos          = 0;
            bool        replaced     = false;
            while ((pos = newName.find(args.pattern, pos)) != std::string::npos) {
                newName.replace(pos, args.pattern.length(), args.replacement);
                pos += args.replacement.length();
                replaced = true;
            }

            if (replaced) {
                if (!args.quietMode) {
                    outs() << "[CATEGORY] Found: " << originalName << " at file offset "
                           << RealNameOffset << "\n"
                           << "  -> Replaced with: " << newName << "\n";
                }
                char* PatchLocation = WritableMB.getBufferStart() + RealNameOffset;
                memcpy(PatchLocation, newName.c_str(), originalName.length());
            }
        } else { // Randomization mode
            if (!args.quietMode)
                outs() << "[CATEGORY] Found: " << CategoryName.str() << " at file offset "
                       << RealNameOffset << "\n";

            std::string RandomString  = generateRandomString(CategoryName.size());
            char*       PatchLocation = WritableMB.getBufferStart() + RealNameOffset;
            memcpy(PatchLocation, RandomString.c_str(), RandomString.length());

            if (!args.quietMode)
                outs() << "  -> Replaced with: " << RandomString << "\n";
        }
    }
}

// Processes a single Mach-O binary slice.
// Now accepts the CommandLineArgs struct.
Error patchMachOSlice(MachOObjectFile*       MachOObj,
                      const MemoryBuffer&    OriginalMB,
                      WritableMemoryBuffer&  WritableMB,
                      uint64_t               SliceOffset,
                      const CommandLineArgs& args)
{
    if (!args.quietMode) {
        outs() << "--- Patching architecture: " << MachOObj->getArchTriple().getArchName()
               << " (slice offset: " << SliceOffset << ") ---\n";
    }
    for (const SectionRef& Section : MachOObj->sections()) {
        Expected<StringRef> SectionNameOrErr = Section.getName();
        if (auto E = SectionNameOrErr.takeError()) {
            return E;
        }
        StringRef SectionName = *SectionNameOrErr;

        if (SectionName == "__objc_classname") {
            patchClassNameSection(Section, MachOObj, WritableMB, SliceOffset, args);
        } else if (SectionName == "__objc_catlist") {
            patchCategoryListSection(Section, MachOObj, OriginalMB, WritableMB, SliceOffset, args);
        }
    }
    return Error::success();
}

} // namespace

int main(int argc, char** argv)
{
    // All command line parsing is now handled in this function.
    auto argsOpt = parseCommandLine(argc, argv);
    if (!argsOpt) {
        // Error message or help text was already printed by the parser.
        return 1;
    }
    // Use the returned struct for all arguments.
    const auto& args = *argsOpt;

    Expected<OwningBinary<Binary>> BinOrErr = createBinary(args.binaryPath);
    if (auto E = BinOrErr.takeError()) {
        errs() << "Error opening binary: " << toString(std::move(E)) << "\n";
        return 1;
    }
    OwningBinary<Binary>& Bin = BinOrErr.get();

    ErrorOr<std::unique_ptr<MemoryBuffer>> MBOrErr = MemoryBuffer::getFile(args.binaryPath);
    if (std::error_code EC = MBOrErr.getError()) {
        errs() << "Error reading file into buffer: " << EC.message() << "\n";
        return 1;
    }
    std::unique_ptr<MemoryBuffer>         OriginalMB {std::move(MBOrErr.get())};
    std::unique_ptr<WritableMemoryBuffer> WritableMB
        = WritableMemoryBuffer::getNewMemBuffer(OriginalMB->getBufferSize());
    memcpy(WritableMB->getBufferStart(), OriginalMB->getBufferStart(), OriginalMB->getBufferSize());

    if (auto* MachOUni = dyn_cast<MachOUniversalBinary>(Bin.getBinary())) {
        for (const auto& ObjForArch : MachOUni->objects()) {
            Expected<std::unique_ptr<MachOObjectFile>> MachOObjOrErr = ObjForArch.getAsObjectFile();
            if (auto E = MachOObjOrErr.takeError()) {
                errs() << "Failed to get object for architecture: " << toString(std::move(E))
                       << "\n";
                continue;
            }
            if (auto E = patchMachOSlice(
                    MachOObjOrErr->get(), *OriginalMB, *WritableMB, ObjForArch.getOffset(), args)) {
                errs() << "Failed to patch Mach-O slice: " << toString(std::move(E)) << "\n";
            }
        }
    } else if (auto* MachOObj = dyn_cast<MachOObjectFile>(Bin.getBinary())) {
        if (auto E = patchMachOSlice(MachOObj, *OriginalMB, *WritableMB, 0, args)) {
            errs() << "Failed to patch Mach-O file: " << toString(std::move(E)) << "\n";
            return 1;
        }
    } else {
        errs() << "The provided file is not a valid Mach-O binary.\n";
        return 1;
    }

    if (args.dryRun) {
        if (!args.quietMode)
            outs() << "\nDry run complete. Binary was not modified.\n";
        return 0;
    }

    // Overwrite the original file with the modified buffer.
    std::error_code EC;
    raw_fd_ostream  OutFile(args.binaryPath, EC);
    if (EC) {
        errs() << "Error opening file for writing: " << EC.message() << "\n";
        return 1;
    }
    OutFile.write(WritableMB->getBufferStart(), WritableMB->getBufferSize());
    OutFile.close();

    if (!args.quietMode)
        outs() << "\nSuccessfully patched binary in-place: " << args.binaryPath << "\n";

    return 0;
}

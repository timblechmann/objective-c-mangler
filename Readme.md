# Objective-C Mangler

A command-line tool for mangling (randomizing or replacing) Objective-C class and category names within a Mach-O binary. This is useful for obfuscation and for preventing namespace conflicts.

The tool operates directly on the binary file, modifying the `__objc_classname` and `__objc_catlist` sections. It supports both single-architecture and universal (fat) binaries.

## Features

- **Randomization**: Replaces Objective-C class and category names with random alphanumeric strings of the same length.
- **Replacement**: Replaces occurrences of a specific string pattern with a replacement string.
- **Exclusion**: Allows specific class names to be excluded from modification.
- **In-place Patching**: Modifies the binary file directly.
- **Dry Run**: Simulates the patching process without writing changes to the file.
- **Support for Universal Binaries**: Correctly handles Mach-O files containing multiple architecture slices.

## Building

The project uses CMake for building.

### Prerequisites

-   CMake (version 3.16 or later)
-   A C++20 compliant compiler (e.g., Clang, GCC)
-   LLVM/Clang libraries (the tool depends on LLVM's Object library).

The `CMakeLists.txt` file is configured to automatically download dependencies:
1.  **CPM.cmake**: For package management.
2.  **CLI11**: For command-line argument parsing.
3.  **LLVM/Clang**: If not found on the system (e.g., via Homebrew), it will download a pre-built version from download.qt.io.

### Build Steps

1.  **Configure the project:**
    ```sh
    cmake -S . -B build
    ```

2.  **Build the executable:**
    ```sh
    cmake --build build
    ```

The executable `objective-c-mangler` will be located in the `build` directory.

## Usage

### Command-Line Interface

```sh

A tool to patch Objective-C metadata in Mach-O binaries.


objective-c-mangler [OPTIONS] binary_to_patch

POSITIONALS:
  binary_to_patch TEXT:FILE REQUIRED
                              The binary file to patch

OPTIONS:
  -h,     --help              Print this help message and exit
          --quiet             Suppress output messages
          --dry-run           Perform a dry run without modifying the file
          --exclude CLASS ... List of class names to exclude from patching
          --replace PATTERN REPLACEMENT x 2
                              Replace a pattern with a replacement string
```

### Examples

-   **Randomize all class names in a binary:**
    ```sh
    ./objective-c-mangler /path/to/your/app
    ```

-   **Perform a dry run to see what would be changed:**
    ```sh
    ./objective-c-mangler --dry-run /path/to/your/app
    ```

-   **Replace a specific namespace prefix:**
    *(Note: For binary safety, the pattern and replacement strings must be the same length.)*
    ```sh
    ./objective-c-mangler --replace "MyPrefix" "NewAlias" /path/to/your/app
    ```

-   **Randomize names but exclude certain critical classes:**
    ```sh
    ./objective-c-mangler --exclude AppDelegate MyCriticalClass /path/to/your/app
    ```

### CMake Integration

You can easily integrate this tool into your own CMake-based project using `FetchContent`. This is particularly useful for applying obfuscation as a post-build step.

The following CMake function demonstrates how to fetch the `objective-c-mangler` project and set up a post-build command to replace a namespace.

```cmake
# Function to fetch and build the objective-c-mangler tool
function(get_objc_mangler)
    # Ensure we build for the host architecture when cross-compiling
    set(CMAKE_OSX_ARCHITECTURES "")
    include(FetchContent)

    FetchContent_Declare(objective-c-mangler
        GIT_REPOSITORY git@github.com:your-repo/objective-c-mangler.git # Change to your repo URL
        GIT_TAG        main
    )
    FetchContent_MakeAvailable(objective-c-mangler)
endfunction()

# --- In your project ---

# Fetch the tool
get_objc_mangler()

# Example of replacing a namespace in your target
if (MY_NAMESPACE)
    # Generate a random string of the same length as the namespace
    string(LENGTH "${MY_NAMESPACE}" input_len)
    string(RANDOM LENGTH ${input_len} ALPHABET "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789" UNIQUE_NAMESPACE)

    add_custom_command(TARGET MyTarget POST_BUILD
        # Step 1: Mangle the binary by replacing the namespace
        COMMAND "$<TARGET_FILE:objective-c-mangler>"
                --replace "${MY_NAMESPACE}" "${UNIQUE_NAMESPACE}"
                "$<TARGET_FILE:MyTarget>"

        # Step 2: Re-sign the binary after modification
        COMMAND codesign --force --sign "-" "$<TARGET_FILE:MyTarget>"
        VERBATIM
        COMMENT "Mangling Objective-C symbols in ${MY_NAMESPACE}"
    )
endif()

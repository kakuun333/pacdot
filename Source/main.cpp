#include "glob-cpp/glob.h"
#include "toml++/toml.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct DotfileDefinition
{
    std::string Name;
    std::vector<fs::path> Paths;
    std::vector<fs::path> Excludes;
    bool BackupEmptyFiles = true;
    bool BackupEmptyDirectories = true;
};

struct Config
{
    fs::path Path;
    std::vector<DotfileDefinition> Dotfiles;
    bool BackupSystemdEnabledUnits = false;
    bool BackupSystemdUserEnabledUnits = false;
    bool BackupPackages = false;
    std::vector<std::string> ExcludedPacmanPackages;
    std::vector<std::string> ExcludedAurPackages;
    std::vector<std::string> ExcludedFlatpakPackages;
};

fs::path GetExecutableDirectory(const char* Argv0)
{

    if (const fs::path ProcExe = "/proc/self/exe"; fs::exists(ProcExe))
    {
        return fs::read_symlink(ProcExe).parent_path();
    }

    fs::path ExecutablePath = Argv0;

    if (ExecutablePath.is_relative())
    {
        ExecutablePath = fs::current_path() / ExecutablePath;
    }

    return fs::weakly_canonical(ExecutablePath).parent_path();
}

std::optional<fs::path> FindConfigPath(const fs::path& ExecutableDirectory)
{
    const std::vector<fs::path> Candidates = {
        fs::current_path() / "pacdot.toml",
        ExecutableDirectory / "pacdot.toml",
        ExecutableDirectory / "pacdot-export/pacdot.toml",
    };

    for (const auto& Candidate : Candidates)
    {
        if (fs::exists(Candidate))
        {
            return Candidate;
        }
    }

    return std::nullopt;
}

DotfileDefinition& GetOrCreateDotfile(Config& ConfigValue, const std::string& Name)
{
    for (auto& Dotfile : ConfigValue.Dotfiles)
    {
        if (Dotfile.Name == Name)
        {
            return Dotfile;
        }
    }

    ConfigValue.Dotfiles.push_back({ Name, {}, {}, true, true });
    return ConfigValue.Dotfiles.back();
}

bool LoadStringArray(const toml::table& Table, const std::string_view Key, std::vector<fs::path>& OutPaths)
{
    const auto* Array = Table[Key].as_array();

    if (Array == nullptr)
    {
        return false;
    }

    for (const auto& Item : *Array)
    {

        if (const auto Value = Item.value<std::string>())
        {
            OutPaths.emplace_back(*Value);
        }
    }

    return true;
}

bool LoadStringArray(const toml::table& Table, const std::string_view Key, std::vector<std::string>& OutValues)
{
    const auto* Array = Table[Key].as_array();

    if (Array == nullptr)
    {
        return false;
    }

    for (const auto& Item : *Array)
    {
        if (const auto Value = Item.value<std::string>())
        {
            OutValues.push_back(*Value);
        }
    }

    return true;
}

bool ReadBoolOrDefault(const toml::table& Table, const std::string_view Key, const bool DefaultValue)
{
    if (const auto* Value = Table.get_as<bool>(Key))
    {
        return Value->get();
    }

    return DefaultValue;
}

std::optional<Config> LoadConfig(const fs::path& ExecutableDirectory)
{
    const auto ConfigPath = FindConfigPath(ExecutableDirectory);

    if (!ConfigPath)
    {
        std::cerr << "Could not find pacdot.toml next to the executable or in the current directory.\n";
        return std::nullopt;
    }

    toml::table ParsedConfig;

    try
    {
        ParsedConfig = toml::parse_file(ConfigPath->string());
    }
    catch (const toml::parse_error& Error)
    {
        std::cerr << "Could not parse " << *ConfigPath << ": " << Error.description() << '\n';
        return std::nullopt;
    }

    Config LoadedConfig;
    LoadedConfig.Path = *ConfigPath;

    if (const auto* DotfilesTable = ParsedConfig["dotfiles"].as_table())
    {
        for (const auto& [Name, Node] : *DotfilesTable)
        {
            const auto* DotfileTable = Node.as_table();

            if (DotfileTable == nullptr)
            {
                continue;
            }

            auto& Dotfile = GetOrCreateDotfile(LoadedConfig, std::string{ Name.str() });

            LoadStringArray(*DotfileTable, "paths", Dotfile.Paths);
            LoadStringArray(*DotfileTable, "exclude", Dotfile.Excludes);
            LoadStringArray(*DotfileTable, "excludes", Dotfile.Excludes);

            Dotfile.BackupEmptyFiles = ReadBoolOrDefault(*DotfileTable, "backup_empty_files", true);
            Dotfile.BackupEmptyDirectories = ReadBoolOrDefault(*DotfileTable, "backup_empty_dirs", true);
        }
    }

    if (const auto* SystemdTable = ParsedConfig["systemd"].as_table())
    {
        LoadedConfig.BackupSystemdEnabledUnits = ReadBoolOrDefault(*SystemdTable, "backup_enabled_units", false);
        LoadedConfig.BackupSystemdUserEnabledUnits = ReadBoolOrDefault(*SystemdTable, "backup_user_enabled_units", false);
    }

    if (const auto* PackagesTable = ParsedConfig["packages"].as_table())
    {
        LoadedConfig.BackupPackages = ReadBoolOrDefault(*PackagesTable, "backup", false);
        LoadStringArray(*PackagesTable, "exclude_pacman", LoadedConfig.ExcludedPacmanPackages);
        LoadStringArray(*PackagesTable, "exclude_aur", LoadedConfig.ExcludedAurPackages);
        LoadStringArray(*PackagesTable, "exclude_flatpak", LoadedConfig.ExcludedFlatpakPackages);
    }

    return LoadedConfig;
}

fs::path ExpandConfigPath(const fs::path& Path, const fs::path& Home)
{
    const std::string PathString = Path.string();

    if (PathString == "~")
    {
        return Home;
    }

    if (PathString.starts_with("~/"))
    {
        return Home / PathString.substr(2);
    }

    if (Path.is_absolute())
    {
        return Path;
    }

    return Home / Path;
}

fs::path BackupRelativePathFor(const fs::path& Source, const fs::path& Home)
{
    const fs::path NormalizedSource = Source.lexically_normal();
    const fs::path NormalizedHome = Home.lexically_normal();

    if (const fs::path RelativeToHome = NormalizedSource.lexically_relative(NormalizedHome); !RelativeToHome.empty() && *RelativeToHome.begin() != "..")
    {
        return RelativeToHome;
    }

    if (Source.is_absolute())
    {
        return Source.relative_path();
    }

    return Source;
}

bool PathMatchesPattern(const fs::path& Path, const fs::path& Pattern)
{
    const fs::path NormalizedPath = Path.lexically_normal();
    const fs::path NormalizedPattern = Pattern.lexically_normal();

    if (const std::string PatternString = NormalizedPattern.string(); PatternString.find_first_of("*?") != std::string::npos)
    {
        glob::glob GlobPattern(PatternString);
        return glob::glob_match(NormalizedPath.string(), GlobPattern);
    }

    const fs::path Relative = NormalizedPath.lexically_relative(NormalizedPattern);
    return Relative.empty() || (!Relative.empty() && *Relative.begin() != "..");
}

bool IsExcluded(const fs::path& Source, const fs::path& Destination, const std::vector<fs::path>& SourceExcludes, const std::vector<fs::path>& DestinationExcludes)
{
    for (const auto& Exclude : SourceExcludes)
    {
        if (PathMatchesPattern(Source, Exclude))
        {
            return true;
        }
    }

    for (const auto& Exclude : DestinationExcludes)
    {
        if (PathMatchesPattern(Destination, Exclude))
        {
            return true;
        }
    }

    return false;
}

bool CopyFileReplacing(const fs::path& Source, const fs::path& Destination)
{
    std::error_code Error;
    fs::create_directories(Destination.parent_path(), Error);

    if (Error)
    {
        std::cerr << "Could not create " << Destination.parent_path() << ": " << Error.message() << '\n';
        return false;
    }

    fs::remove_all(Destination, Error);

    if (Error)
    {
        std::cerr << "Could not remove " << Destination << ": " << Error.message() << '\n';
        return false;
    }

    fs::copy_file(Source, Destination, fs::copy_options::overwrite_existing, Error);

    if (Error)
    {
        std::cerr << "Could not copy " << Source << " -> " << Destination << ": " << Error.message() << '\n';
        return false;
    }

    std::cout << "[COPY] " << Source << " -> " << Destination << '\n';
    return true;
}

bool CopySymlinkReplacing(const fs::path& Source, const fs::path& Destination)
{
    std::error_code Error;
    fs::create_directories(Destination.parent_path(), Error);

    if (Error)
    {
        std::cerr << "Could not create " << Destination.parent_path() << ": " << Error.message() << '\n';
        return false;
    }

    fs::remove_all(Destination, Error);

    if (Error)
    {
        std::cerr << "Could not remove " << Destination << ": " << Error.message() << '\n';
        return false;
    }

    fs::copy_symlink(Source, Destination, Error);

    if (Error)
    {
        std::cerr << "Could not copy symlink " << Source << " -> " << Destination << ": " << Error.message() << '\n';
        return false;
    }

    std::cout << "[COPY] " << Source << " -> " << Destination << '\n';
    return true;
}

bool IsEmptyRegularFile(const fs::path& Path)
{
    std::error_code Error;

    if (!fs::is_regular_file(Path, Error) || Error)
    {
        return false;
    }

    return fs::file_size(Path, Error) == 0 && !Error;
}

std::string ShellQuote(const fs::path& Path)
{
    std::string Quoted = "'";

    for (const char Character : Path.string())
    {
        if (Character == '\'')
        {
            Quoted += "'\\''";
        }
        else
        {
            Quoted += Character;
        }
    }

    Quoted += "'";
    return Quoted;
}

std::string ShellQuote(const std::string& Value)
{
    return ShellQuote(fs::path(Value));
}

int RunCommand(const std::string& Command)
{
    std::cout << "[RUN] " << Command << '\n';
    return std::system(Command.c_str());
}

fs::path BackupPathFor(const fs::path& Root, const std::string& Name, const fs::path& Source, const fs::path& Home)
{
    return Root / "dotfiles" / Name / BackupRelativePathFor(Source, Home);
}

std::vector<fs::path> BuildSourceExcludes(const DotfileDefinition& Dotfile, const fs::path& Home)
{
    std::vector<fs::path> Excludes;

    for (const auto& Exclude : Dotfile.Excludes)
    {
        Excludes.push_back(ExpandConfigPath(Exclude, Home));
    }

    return Excludes;
}

std::vector<fs::path> BuildBackupExcludes(const DotfileDefinition& Dotfile, const fs::path& Root, const fs::path& Home)
{
    std::vector<fs::path> Excludes;

    for (const auto& Exclude : Dotfile.Excludes)
    {
        Excludes.push_back(BackupPathFor(Root, Dotfile.Name, ExpandConfigPath(Exclude, Home), Home));
    }

    return Excludes;
}

bool CopyPath(
    const fs::path& Source,
    const fs::path& Destination,
    const std::vector<fs::path>& SourceExcludes,
    const std::vector<fs::path>& DestinationExcludes,
    const bool bDryRun,
    const bool IncludeEmptyFiles,
    const bool IncludeEmptyDirectories)
{
    if (!fs::exists(Source))
    {
        std::cout << "[SKIP] " << Source << " does not exist.\n";
        return true;
    }

    if (IsExcluded(Source, Destination, SourceExcludes, DestinationExcludes))
    {
        std::cout << "[SKIP] " << Source << " is excluded.\n";
        return true;
    }

    if (!IncludeEmptyFiles && IsEmptyRegularFile(Source))
    {
        std::cout << "[SKIP] " << Source << " is empty.\n";
        return true;
    }

    if (bDryRun && (!fs::is_directory(Source) || IncludeEmptyDirectories))
    {
        std::cout << "[DRY-RUN] Would copy " << Source << " -> " << Destination << '\n';
    }

    if (fs::is_directory(Source))
    {
        std::error_code Error;

        if (!bDryRun && IncludeEmptyDirectories)
        {
            fs::remove_all(Destination, Error);

            if (Error)
            {
                std::cerr << "Could not remove " << Destination << ": " << Error.message() << '\n';
                return false;
            }

            fs::create_directories(Destination, Error);

            if (Error)
            {
                std::cerr << "Could not create " << Destination << ": " << Error.message() << '\n';
                return false;
            }
        }
        else if (!bDryRun)
        {
            fs::remove_all(Destination, Error);

            if (Error)
            {
                std::cerr << "Could not remove " << Destination << ": " << Error.message() << '\n';
                return false;
            }
        }

        bool bSuccess = true;

        for (fs::recursive_directory_iterator It(Source, fs::directory_options::skip_permission_denied, Error), End; It != End; It.increment(Error))
        {
            if (Error)
            {
                std::cerr << "Could not read " << Source << ": " << Error.message() << '\n';
                return false;
            }

            const fs::path EntrySource = It->path();
            const fs::path EntryDestination = Destination / EntrySource.lexically_relative(Source);

            if (IsExcluded(EntrySource, EntryDestination, SourceExcludes, DestinationExcludes))
            {
                std::cout << "[SKIP] " << EntrySource << " is excluded.\n";

                if (It->is_directory())
                {
                    It.disable_recursion_pending();
                }

                continue;
            }

            if (!IncludeEmptyFiles && IsEmptyRegularFile(EntrySource))
            {
                std::cout << "[SKIP] " << EntrySource << " is empty.\n";
                continue;
            }

            if (bDryRun)
            {
                if (!It->is_directory() || IncludeEmptyDirectories)
                {
                    std::cout << "[DRY-RUN] Would copy " << EntrySource << " -> " << EntryDestination << '\n';
                }

                continue;
            }

            if (It->is_directory())
            {
                if (IncludeEmptyDirectories)
                {
                    fs::create_directories(EntryDestination, Error);

                    if (Error)
                    {
                        std::cerr << "Could not create " << EntryDestination << ": " << Error.message() << '\n';
                        bSuccess = false;
                    }
                }
            }
            else if (It->is_symlink())
            {
                bSuccess = CopySymlinkReplacing(EntrySource, EntryDestination) && bSuccess;
            }
            else if (It->is_regular_file())
            {
                bSuccess = CopyFileReplacing(EntrySource, EntryDestination) && bSuccess;
            }
        }

        return bSuccess;
    }

    if (bDryRun)
    {
        return true;
    }

    if (fs::is_symlink(Source))
    {
        return CopySymlinkReplacing(Source, Destination);
    }

    return CopyFileReplacing(Source, Destination);
}

int ExportDotfiles(const fs::path& Root, const Config& ConfigValue)
{
    const char* HomeValue = std::getenv("HOME");

    if (HomeValue == nullptr)
    {
        std::cerr << "HOME is not set.\n";
        return 1;
    }

    const fs::path Home = HomeValue;
    int Result = 0;

    for (const auto& Dotfile : ConfigValue.Dotfiles)
    {
        const std::vector<fs::path> SourceExcludes = BuildSourceExcludes(Dotfile, Home);
        const std::vector<fs::path> DestinationExcludes = BuildBackupExcludes(Dotfile, Root, Home);

        for (const auto& ConfigPath : Dotfile.Paths)
        {
            const fs::path Source = ExpandConfigPath(ConfigPath, Home);

            if (const fs::path Destination = BackupPathFor(Root, Dotfile.Name, Source, Home); !CopyPath(
                    Source,
                    Destination,
                    SourceExcludes,
                    DestinationExcludes,
                    false,
                    Dotfile.BackupEmptyFiles,
                    Dotfile.BackupEmptyDirectories))
            {
                Result = 1;
            }
        }
    }

    if (Result == 0)
    {
        std::cout << "Dotfiles exported successfully.\n";
    }

    return Result;
}

int RestoreDotfiles(const fs::path& Root, const Config& ConfigValue, const bool bDryRun)
{
    const char* HomeValue = std::getenv("HOME");

    if (HomeValue == nullptr)
    {
        std::cerr << "HOME is not set.\n";
        return 1;
    }

    const fs::path Home = HomeValue;
    int Result = 0;

    for (const auto& Dotfile : ConfigValue.Dotfiles)
    {
        const std::vector<fs::path> SourceExcludes = BuildBackupExcludes(Dotfile, Root, Home);
        const std::vector<fs::path> DestinationExcludes = BuildSourceExcludes(Dotfile, Home);

        for (const auto& ConfigPath : Dotfile.Paths)
        {
            const fs::path Destination = ExpandConfigPath(ConfigPath, Home);

            if (const fs::path Source = BackupPathFor(Root, Dotfile.Name, Destination, Home); !CopyPath(Source, Destination, SourceExcludes, DestinationExcludes, bDryRun, true, true))
            {
                Result = 1;
            }
        }
    }

    if (Result == 0)
    {
        std::cout << "Dotfiles restored successfully.\n";
    }

    return Result;
}

fs::path SystemdEnabledUnitsPath(const fs::path& Root, const bool User)
{
    return Root / "systemd" / (User ? "user-enabled-units.txt" : "system-enabled-units.txt");
}

int ExportSystemdEnabledUnits(const fs::path& Root, const bool User)
{
    std::error_code Error;
    const fs::path UnitsPath = SystemdEnabledUnitsPath(Root, User);
    fs::create_directories(UnitsPath.parent_path(), Error);

    if (Error)
    {
        std::cerr << "Could not create " << UnitsPath.parent_path() << ": " << Error.message() << '\n';
        return 1;
    }

    const std::string Command = std::string("systemctl ") + (User ? "--user " : "") + "list-unit-files --state=enabled --no-legend --no-pager | awk '{print $1}' > " + ShellQuote(UnitsPath);

    if (RunCommand(Command) != 0)
    {
        std::cerr << "Could not export " << (User ? "user " : "") << "systemd enabled units.\n";
        return 1;
    }

    std::cout << (User ? "User systemd" : "Systemd") << " enabled units exported successfully.\n";
    return 0;
}

std::vector<std::string> ReadLines(const fs::path& Path)
{
    std::vector<std::string> Lines;
    std::ifstream File(Path);
    std::string Line;

    while (std::getline(File, Line))
    {
        if (!Line.empty())
        {
            Lines.push_back(Line);
        }
    }

    return Lines;
}

bool Contains(const std::vector<std::string>& Values, const std::string& Value)
{
    for (const auto& CurrentValue : Values)
    {
        if (CurrentValue == Value)
        {
            return true;
        }
    }

    return false;
}

std::vector<std::string> FilterExcluded(const std::vector<std::string>& Values, const std::vector<std::string>& Excludes)
{
    std::vector<std::string> FilteredValues;

    for (const auto& Value : Values)
    {
        if (!Contains(Excludes, Value))
        {
            FilteredValues.push_back(Value);
        }
    }

    return FilteredValues;
}

bool ReadCommandLines(const std::string& Command, std::vector<std::string>& OutLines)
{
    std::array<char, 256> Buffer{};
    FILE* Pipe = popen(Command.c_str(), "r");

    if (Pipe == nullptr)
    {
        std::cerr << "Could not run command: " << Command << '\n';
        return false;
    }

    while (fgets(Buffer.data(), static_cast<int>(Buffer.size()), Pipe) != nullptr)
    {
        std::string Line = Buffer.data();

        while (!Line.empty() && (Line.back() == '\n' || Line.back() == '\r'))
        {
            Line.pop_back();
        }

        if (!Line.empty())
        {
            OutLines.push_back(Line);
        }
    }

    if (pclose(Pipe) != 0)
    {
        std::cerr << "Command returned a non-zero status: " << Command << '\n';
        return false;
    }

    return true;
}

bool WriteLines(const fs::path& Path, const std::vector<std::string>& Lines)
{
    std::error_code Error;
    fs::create_directories(Path.parent_path(), Error);

    if (Error)
    {
        std::cerr << "Could not create " << Path.parent_path() << ": " << Error.message() << '\n';
        return false;
    }

    std::ofstream File(Path);

    if (!File)
    {
        std::cerr << "Could not write " << Path << '\n';
        return false;
    }

    for (const auto& Line : Lines)
    {
        File << Line << '\n';
    }

    return true;
}

fs::path PackageListPath(const fs::path& Root, const std::string& Name)
{
    return Root / "packages" / (Name + ".txt");
}

int ExportPackageList(
    const fs::path& Root,
    const std::string& Name,
    const std::string& Command,
    const std::vector<std::string>& Excludes)
{
    std::vector<std::string> CommandOutput;

    if (!ReadCommandLines(Command, CommandOutput))
    {
        return 1;
    }

    const std::vector<std::string> Packages = FilterExcluded(CommandOutput, Excludes);
    const fs::path Destination = PackageListPath(Root, Name);

    if (!WriteLines(Destination, Packages))
    {
        return 1;
    }

    std::cout << Name << " packages exported to " << Destination << ".\n";
    return 0;
}

int ExportPackages(const fs::path& Root, const Config& ConfigValue)
{
    if (!ConfigValue.BackupPackages)
    {
        std::cout << "[SKIP] Package backup is disabled in pacdot.toml.\n";
        return 0;
    }

    int Result = 0;
    Result |= ExportPackageList(Root, "pacman", "pacman -Qqen", ConfigValue.ExcludedPacmanPackages);
    Result |= ExportPackageList(Root, "aur", "pacman -Qqem", ConfigValue.ExcludedAurPackages);
    Result |= ExportPackageList(Root, "flatpak", "flatpak list --app --columns=application", ConfigValue.ExcludedFlatpakPackages);
    return Result == 0 ? 0 : 1;
}

std::string JoinShellArgs(const std::vector<std::string>& Values)
{
    std::string Joined;

    for (const auto& Value : Values)
    {
        if (!Joined.empty())
        {
            Joined += ' ';
        }

        Joined += ShellQuote(Value);
    }

    return Joined;
}

int RestorePackageList(
    const fs::path& Root,
    const std::string& Name,
    const std::string& InstallPrefix,
    const bool DryRun)
{
    const fs::path Source = PackageListPath(Root, Name);

    if (!fs::exists(Source))
    {
        std::cout << "[SKIP] " << Source << " does not exist.\n";
        return 0;
    }

    const std::vector<std::string> Packages = ReadLines(Source);

    if (Packages.empty())
    {
        std::cout << "[SKIP] " << Source << " is empty.\n";
        return 0;
    }

    const std::string Command = InstallPrefix + JoinShellArgs(Packages);

    if (DryRun)
    {
        std::cout << "[DRY-RUN] " << Command << '\n';
        return 0;
    }

    return RunCommand(Command) == 0 ? 0 : 1;
}

int RestorePackages(const fs::path& Root, const Config& ConfigValue, const bool DryRun)
{
    if (!ConfigValue.BackupPackages)
    {
        std::cout << "[SKIP] Package backup is disabled in pacdot.toml.\n";
        return 0;
    }

    int Result = 0;
    Result |= RestorePackageList(Root, "pacman", "sudo pacman -S --needed -- ", DryRun);
    Result |= RestorePackageList(Root, "aur", "paru -S --needed -- ", DryRun);
    Result |= RestorePackageList(Root, "flatpak", "flatpak install -y flathub ", DryRun);
    return Result == 0 ? 0 : 1;
}

int RestoreSystemdEnabledUnits(const fs::path& Root, const bool User, const bool DryRun)
{
    const fs::path UnitsPath = SystemdEnabledUnitsPath(Root, User);

    if (!fs::exists(UnitsPath))
    {
        std::cout << "[SKIP] " << UnitsPath << " does not exist.\n";
        return 0;
    }

    int Result = 0;

    for (const auto& Unit : ReadLines(UnitsPath))
    {
        const std::string Command = std::string("systemctl ") + (User ? "--user " : "") + "enable " + ShellQuote(Unit);

        if (DryRun)
        {
            std::cout << "[DRY-RUN] " << Command << '\n';
            continue;
        }

        Result |= RunCommand(Command);
    }

    if (Result != 0)
    {
        std::cerr << "Could not restore all " << (User ? "user " : "") << "systemd enabled units.\n";
        return 1;
    }

    std::cout << (User ? "User systemd" : "Systemd") << " enabled units restored successfully.\n";
    return 0;
}

int ExportAll(const fs::path& Root, const Config& ConfigValue)
{
    std::error_code Error;
    fs::create_directories(Root, Error);

    if (Error)
    {
        std::cerr << "Could not create " << Root << ": " << Error.message() << '\n';
        return 1;
    }

    if (const fs::path ExportedConfig = Root / "pacdot.toml"; !fs::equivalent(ConfigValue.Path, ExportedConfig, Error))
    {
        Error.clear();
        fs::copy_file(ConfigValue.Path, ExportedConfig, fs::copy_options::overwrite_existing, Error);

        if (Error)
        {
            std::cerr << "Could not copy " << ConfigValue.Path << " -> " << ExportedConfig << ": " << Error.message() << '\n';
            return 1;
        }
    }

    int Result = 0;
    Result |= ExportDotfiles(Root, ConfigValue);

    if (ConfigValue.BackupSystemdEnabledUnits)
    {
        Result |= ExportSystemdEnabledUnits(Root, false);
    }

    if (ConfigValue.BackupSystemdUserEnabledUnits)
    {
        Result |= ExportSystemdEnabledUnits(Root, true);
    }

    if (ConfigValue.BackupPackages)
    {
        Result |= ExportPackages(Root, ConfigValue);
    }

    return Result == 0 ? 0 : 1;
}

int RestoreAll(const fs::path& Root, const Config& ConfigValue, const bool DryRun, const bool InstallPackages)
{
    int Result = 0;
    Result |= RestoreDotfiles(Root, ConfigValue, DryRun);

    if (ConfigValue.BackupSystemdEnabledUnits)
    {
        Result |= RestoreSystemdEnabledUnits(Root, false, DryRun);
    }

    if (ConfigValue.BackupSystemdUserEnabledUnits)
    {
        Result |= RestoreSystemdEnabledUnits(Root, true, DryRun);
    }

    if (ConfigValue.BackupPackages)
    {
        if (InstallPackages)
        {
            Result |= RestorePackages(Root, ConfigValue, DryRun);
        }
        else
        {
            std::cout << "[SKIP] Package installation skipped. Use --install-packages to install backed up packages.\n";
        }
    }

    return Result == 0 ? 0 : 1;
}

void PrintHelp()
{
    std::cout << "Usage:\n";
    std::cout << "  pacdot --help\n";
    std::cout << "  pacdot export\n";
    std::cout << "  pacdot restore [--dry-run] [--install-packages]\n";
    std::cout << "  pacdot dotfiles export\n";
    std::cout << "  pacdot dotfiles restore [--dry-run]\n";
    std::cout << "  pacdot packages export\n";
    std::cout << "  pacdot packages restore [--dry-run]\n";
    std::cout << '\n';
    std::cout << "Commands:\n";
    std::cout << "  --help, -h                              Show this help message.\n";
    std::cout << "  export                                  Export configured data into pacdot-export.\n";
    std::cout << "  restore [--dry-run] [--install-packages] Restore configured data from pacdot-export.\n";
    std::cout << "  dotfiles export                         Export only configured dotfiles.\n";
    std::cout << "  dotfiles restore [--dry-run]            Restore only configured dotfiles.\n";
    std::cout << "  packages export                         Export only configured package lists.\n";
    std::cout << "  packages restore [--dry-run]            Install only backed up package lists.\n";
    std::cout << '\n';
    std::cout << "Options:\n";
    std::cout << "  --dry-run                               Print restore actions without changing files or installing packages.\n";
    std::cout << "  --install-packages                      Install backed up pacman, AUR, and Flatpak packages during restore.\n";
}

bool HasArg(const int Argc, char* Argv[], const std::string& Expected)
{
    for (int i = 2; i < Argc; ++i)
    {
        if (std::string(Argv[i]) == Expected)
        {
            return true;
        }
    }

    return false;
}

int main(int Argc, char* Argv[])
{
    const fs::path ExecutableDirectory = GetExecutableDirectory(Argv[0]);
    const fs::path ExportRoot = ExecutableDirectory / "pacdot-export";

    if (Argc < 2)
    {
        PrintHelp();
        return 1;
    }

    std::string Command = Argv[1];

    if (Command == "--help" || Command == "-h")
    {
        PrintHelp();
        return 0;
    }

    if (Command == "export")
    {
        const auto LoadedConfig = LoadConfig(ExecutableDirectory);
        return LoadedConfig ? ExportAll(ExportRoot, *LoadedConfig) : 1;
    }

    if (Command == "restore")
    {
        const auto LoadedConfig = LoadConfig(ExecutableDirectory);
        return LoadedConfig ? RestoreAll(ExportRoot, *LoadedConfig, HasArg(Argc, Argv, "--dry-run"), HasArg(Argc, Argv, "--install-packages")) : 1;
    }

    if (Command == "dotfiles")
    {
        if (Argc < 3)
        {
            PrintHelp();
            return 1;
        }

        const auto LoadedConfig = LoadConfig(ExecutableDirectory);

        if (!LoadedConfig)
        {
            return 1;
        }

        const std::string DotfileCommand = Argv[2];

        if (DotfileCommand == "export")
        {
            return ExportDotfiles(ExportRoot, *LoadedConfig);
        }

        if (DotfileCommand == "restore")
        {
            return RestoreDotfiles(ExportRoot, *LoadedConfig, HasArg(Argc, Argv, "--dry-run"));
        }
    }

    if (Command == "packages")
    {
        if (Argc < 3)
        {
            PrintHelp();
            return 1;
        }

        const auto LoadedConfig = LoadConfig(ExecutableDirectory);

        if (!LoadedConfig)
        {
            return 1;
        }

        const std::string PackagesCommand = Argv[2];

        if (PackagesCommand == "export")
        {
            return ExportPackages(ExportRoot, *LoadedConfig);
        }

        if (PackagesCommand == "restore")
        {
            return RestorePackages(ExportRoot, *LoadedConfig, HasArg(Argc, Argv, "--dry-run"));
        }
    }

    std::cerr << "Unknown command: " << Command << '\n';
    std::cerr << "Run 'pacdot --help' for usage.\n";
    return 1;
}

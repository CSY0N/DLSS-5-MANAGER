namespace DLSS_5_MANAGER.Services

// What DLSS 5 MANAGER keeps in %LOCALAPPDATA%\DLSS5Manager, and what of it can
// go without anything working differently (round 35):
//
//   * Backups of game files no install record points to any more. Removal
//     puts the originals back and deletes the record, but never deleted the
//     copies - 2 GB of them on one PC. Restores read a backup only through its
//     record (ModInstaller, AdvancedInstall), so one no record names is never
//     read again; worse, a later install reuses a copy by that name instead of
//     backing up the file that is there then. Every record is read - all of
//     AppData's Installs and the one beside each known game - and when any of
//     them cannot be read, no backup is touched at all. At start only the
//     backups of games the app knows are looked at; "Free up space" also takes
//     those of games it has never heard of.
//   * Files nothing reads any more: library_cache.json / _v2, analysis_cache_v1
//     .json, the old NeuralScreen.log, and LIVE FLOW's own DLSS MFG
//     compatibility files (that generator left the page in round 32).
//   * The compatibility loader's logs past the newest few (one pair a run).
//
// "Free up space" in Settings also empties the image caches (PULSE and
// COMMUNITY pictures, fetched again when they are shown) and SMFG's log and
// compatibility caches (made again at its next start).

open System
open System.Collections.Generic
open System.IO
open System.Text.Json

module StorageCleaner =
    let private root =
        match Environment.GetEnvironmentVariable "DLSS5MANAGER_STORAGE_ROOT" with   // tests: a made-up folder
        | null | "" -> Path.Combine(Environment.GetFolderPath Environment.SpecialFolder.LocalApplicationData, "DLSS5Manager")
        | other -> other

    let private sizeOf (path: string) =
        try
            if File.Exists path then FileInfo(path).Length
            elif Directory.Exists path then
                DirectoryInfo(path).EnumerateFiles("*", SearchOption.AllDirectories) |> Seq.sumBy (fun f -> f.Length)
            else 0L
        with _ -> 0L

    /// Deleted as far as it goes; the bytes that went.
    let private remove (path: string) =
        let before = sizeOf path
        try
            if File.Exists path then File.Delete path
            elif Directory.Exists path then Directory.Delete(path, true)
        with _ ->
            // Something in use (a log being written): whatever could go, went.
            if Directory.Exists path then
                for file in Directory.EnumerateFiles(path, "*", SearchOption.AllDirectories) |> Seq.toArray do
                    try File.Delete file with _ -> ()
        before - sizeOf path

    // ---- backups no record points to ------------------------------------------
    /// Every backup path the install records name, or None when a record could not be read (then nothing is
    /// known for certain, and no backup is touched).
    let private backupsInUse (sidecars: string seq) : HashSet<string> option =
        let used = HashSet<string>(StringComparer.OrdinalIgnoreCase)
        let mutable unreadable = false
        let read (record: string) =
            try
                use doc = JsonDocument.Parse(File.ReadAllText record)
                let mutable files = Unchecked.defaultof<JsonElement>
                let found =
                    doc.RootElement.ValueKind = JsonValueKind.Object
                    && (doc.RootElement.TryGetProperty("Files", &files) || doc.RootElement.TryGetProperty("files", &files))
                if found && files.ValueKind = JsonValueKind.Array then
                    for entry in files.EnumerateArray() do
                        let mutable backup = Unchecked.defaultof<JsonElement>
                        if entry.ValueKind = JsonValueKind.Object
                           && (entry.TryGetProperty("BackupPath", &backup) || entry.TryGetProperty("backupPath", &backup))
                           && backup.ValueKind = JsonValueKind.String then
                            let path = backup.GetString()
                            if not (String.IsNullOrWhiteSpace path) then
                                used.Add(try Path.GetFullPath path with _ -> path) |> ignore
            with _ -> unreadable <- true
        let installs = Path.Combine(root, "Installs")
        if Directory.Exists installs then
            for record in Directory.GetFiles(installs, "*.json") do read record
        for sidecar in sidecars do
            if not (String.IsNullOrWhiteSpace sidecar) && File.Exists sidecar then read sidecar
        if unreadable then None else Some used

    /// The games and emulators the app knows (its library caches).
    let private knownGames () =
        (try GameScanner.loadCachedGames () with _ -> []) @ (try GameScanner.loadCachedEmulators () with _ -> [])

    /// Backup files no record names. A copy made in the last hour is left alone: an install writes its backups
    /// before its record. With `unknownToo` off (the quiet tidy at start), only the backups of games the app knows -
    /// a record in AppData, or the game in its library - are looked at; a game it does not know might still have
    /// a record beside it somewhere.
    let private unusedBackups (unknownToo: bool) : string list =
        let backups = Path.Combine(root, "Backups")
        if not (Directory.Exists backups) then []
        else
            let games = knownGames ()
            let sidecars = games |> List.map (fun g -> try ModInstaller.sidecarPath g with _ -> "")
            match backupsInUse sidecars with
            | None -> []
            | Some used ->
                let known = HashSet<string>(StringComparer.OrdinalIgnoreCase)
                for g in games do (try known.Add(ModInstaller.safeId g) |> ignore with _ -> ())
                let installs = Path.Combine(root, "Installs")
                if Directory.Exists installs then
                    for record in Directory.GetFiles(installs, "*.json") do known.Add(Path.GetFileNameWithoutExtension record) |> ignore
                let recent = DateTime.UtcNow.AddHours -1.
                Directory.GetDirectories backups
                |> Array.filter (fun dir -> unknownToo || known.Contains(Path.GetFileName dir))
                |> Seq.collect (fun dir -> Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
                |> Seq.filter (fun file ->
                    not (used.Contains(Path.GetFullPath file))
                    && (try File.GetCreationTimeUtc file < recent && File.GetLastWriteTimeUtc file < recent with _ -> false))
                |> Seq.toList

    // ---- files nothing reads -----------------------------------------------------
    let private deadFiles () =
        [ Path.Combine(root, "library_cache.json")
          Path.Combine(root, "library_cache_v2.json")
          Path.Combine(root, "analysis_cache_v1.json")
          Path.Combine(root, "Screen", "NeuralScreen.log")
          Path.Combine(root, "Screen", "dlssg-compat") ]
        |> List.filter (fun p -> File.Exists p || Directory.Exists p)

    /// SMFG's compatibility loader writes a backend_<pid> and a loader_<pid> log every run; the newest few pairs
    /// are kept. (LIVE FLOW's own copy of that loader goes whole - deadFiles.)
    let private oldLoaderLogs (keep: int) =
        let compat = Path.Combine(SmfgStore.dataFolder, "dlssg-compat")
        if not (Directory.Exists compat) then []
        else
            Directory.GetDirectories(compat, "logs", SearchOption.AllDirectories)
            |> Array.toList
            |> List.collect (fun logs ->
                let files = DirectoryInfo(logs).GetFiles("*.jsonl") |> Array.sortByDescending (fun f -> f.LastWriteTimeUtc)
                if files.Length > keep * 2 then files[keep * 2 ..] |> Array.map (fun f -> f.FullName) |> Array.toList else [])

    /// Game folders in Backups with nothing left in them.
    let private emptyBackupFolders () =
        let backups = Path.Combine(root, "Backups")
        if not (Directory.Exists backups) then 0L
        else
            for dir in Directory.GetDirectories backups do
                try (if Directory.GetFileSystemEntries(dir).Length = 0 then Directory.Delete dir) with _ -> ()
            0L

    // ---- what "Free up space" adds ---------------------------------------------------
    let private imageCaches () =
        [ Path.Combine(root, "Cache", "Pulse"); Path.Combine(root, "Cache", "Community") ]

    let private engineCaches (engineRunning: bool) =
        if engineRunning then []
        else
            [ Path.Combine(SmfgStore.dataFolder, "dlssg-compat")
              Path.Combine(SmfgStore.dataFolder, "history")
              Path.Combine(SmfgStore.dataFolder, "engine.prev.log") ]

    /// SMFG's engine running now (its caches are left alone then).
    let smfgRunning () =
        try
            Diagnostics.Process.GetProcessesByName "nvngx"
            |> Array.exists (fun p -> try p.MainModule.FileName.StartsWith(SmfgStore.folder, StringComparison.OrdinalIgnoreCase) with _ -> false)
        with _ -> false

    /// What is used on this PC, and what of it "Free up space" would free.
    type Report = { Used: int64; Freeable: int64 }

    let measure (engineRunning: bool) =
        let inside = SmfgStore.dataFolder.StartsWith(root, StringComparison.OrdinalIgnoreCase)
        let used = sizeOf root + (if inside then 0L else sizeOf SmfgStore.dataFolder)
        let freeable =
            (unusedBackups true |> List.sumBy sizeOf)
            + (deadFiles () |> List.sumBy sizeOf)
            + (imageCaches () |> List.sumBy sizeOf)
            + (if engineRunning then oldLoaderLogs 6 |> List.sumBy sizeOf else engineCaches false |> List.sumBy sizeOf)
        { Used = used; Freeable = freeable }

    /// What the quiet tidy (false) or "Free up space" (true) would remove - nothing is touched (tests, a dry run).
    let plan (freeUpToo: bool) (engineRunning: bool) =
        deadFiles () @ oldLoaderLogs 6 @ unusedBackups freeUpToo
        @ (if freeUpToo then imageCaches () @ engineCaches engineRunning |> List.filter (fun p -> File.Exists p || Directory.Exists p) else [])

    /// At start, quietly: only what nothing will ever read. The bytes freed.
    let private clean (unknownToo: bool) =
        let freed = (deadFiles () @ oldLoaderLogs 6 @ unusedBackups unknownToo) |> List.sumBy remove
        freed + emptyBackupFolders ()

    let tidy () = clean false

    /// "Free up space": the above, the image caches and SMFG's caches. The bytes freed.
    let freeUp (engineRunning: bool) =
        clean true + (imageCaches () @ engineCaches engineRunning |> List.sumBy remove)

    /// "3.9 GB", "117 MB".
    let text (bytes: int64) =
        if bytes >= 1024L * 1024L * 1024L then sprintf "%.1f GB" (float bytes / 1073741824.)
        elif bytes >= 1024L * 1024L then sprintf "%.0f MB" (float bytes / 1048576.)
        else sprintf "%.0f KB" (float bytes / 1024.)

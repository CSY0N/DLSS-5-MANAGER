namespace DLSS_5_MANAGER.Services

open System
open System.IO
open System.Text.Json
open System.Collections.Generic
open DLSS_5_MANAGER.Models
open DLSS_5_MANAGER.Services.GameAnalyzer
open DLSS_5_MANAGER.Services.ModInstaller

/// The advanced install (Settings -> Features -> Advanced install): every part
/// the one-click install lays down, installed and removed one at a time.
///
/// This is not a second installer. Each step below is a step of the one-click
/// routes - the same ReShade setup, the same payload folders, the same
/// `Tracker`, the same restore point beside the game - cut at the joints
/// between the parts, so any set of them can go in or come out. The routes
/// survive as presets: `recommend` ticks exactly what a route installs.
///
/// Every file an advanced install writes carries its part in the restore point
/// (`InstalledFile.Component`). A record written by the one-click install has
/// no such tag, and `partOf` works the part out from the file itself - so an
/// install made either way can be taken apart here.
module AdvancedInstall =

    // =====================================================================
    // THE PARTS
    // =====================================================================
    let partReShade = "reshade"
    let partEffects = "effects"
    let partDgVoodoo = "dgvoodoo"
    let partHost64 = "host64"
    let partFeed = "feed"
    let partRenoDx = "renodx"
    let partDeepFried = "deepfried"
    let partMfgUnlock = "mfgunlock"
    let partOptiScaler = "optiscaler"
    let partAmd = "amd"
    let partStreamline = "streamline"
    let partDlss = "dlss"
    let partModel = "model"
    let partModelSpread = "modelspread"
    let partNeural = "neural"
    let partOverlay = "overlay"
    let partExtras = "extras"

    /// The order they go in - the one-click routes' own order. ReShade comes
    /// first so it has its slot before OptiScaler and the AMD payload look for
    /// a free one; removal walks this backwards.
    let installOrder =
        [| partReShade; partEffects; partDgVoodoo; partHost64; partFeed; partRenoDx; partDeepFried; partMfgUnlock
           partOptiScaler; partAmd; partStreamline; partDlss; partModel; partModelSpread; partNeural; partOverlay
           partExtras |]

    /// What the user picked for one run, beside the parts themselves.
    type Choices =
        { /// Where the build-dependent parts go: a 32-bit game gets the 32-bit
          /// add-ons beside it and every 64-bit module inside host64.
          Build: InstallArch
          /// "auto" | "dxgi" | "d3d9" | "opengl" | "vulkan"
          ReShadeApi: string
          /// OptiScaler's slot order: DirectX 12 starts at dxgi.dll, Vulkan at winmm.dll.
          OptiApi: OptiScalerApi
          /// "standard" | "multipass"
          RenoDxBuild: string
          /// The overlay's look and key. Whether it goes in at all is its part.
          Overlay: OverlayOptions
          /// The route written into the restore point as its Mode - what the
          /// card's badge, the one-click sheet and the community read - and
          /// the one the user's extras are matched against.
          Basis: InstallMode
          /// An emulator takes ReShade's slot from the catalogue on "auto".
          IsEmulator: bool }

    /// One part as it stands on the game.
    type PartState =
        { Id: string
          /// The files the restore point credits to it.
          Files: string[]
          /// The restore point holds files of this part.
          Installed: bool
          /// ...and a file it added has since gone from the disk.
          Incomplete: bool
          /// Not put there by this app, but already in the game.
          Present: bool
          /// Installing it now would copy nothing, and why: "uptodate",
          /// "nostreamline", "nofolders", "noextras". "" when there is work.
          Idle: string
          /// Why it cannot be installed from here: "missing" (not in "mod
          /// files") or "download" (a cloud add-on not fetched yet).
          Blocked: string
          /// What the copy on the game is, to hold against what is picked:
          /// ReShade's slot, RenoDX's build, OptiScaler's API, "64" / "32".
          Variant: string }

    // =====================================================================
    // SMALL HELPERS
    // =====================================================================
    let private same (a: string) (b: string) = String.Equals(a, b, StringComparison.OrdinalIgnoreCase)

    let private trimDir (d: string) = if isNull d then "" else d.TrimEnd('\\', '/')

    let private sameDir (a: string) (b: string) = same (trimDir a) (trimDir b)

    /// ReShade's file for an API. Vulkan is a layer and has no file beside the game.
    let reshadeSlotFile (api: string) =
        match api with
        | "d3d9" -> "d3d9.dll"
        | "opengl" -> "opengl32.dll"
        | "vulkan" -> ""
        | _ -> "dxgi.dll"

    /// "auto" made concrete, the way each route decides it.
    let resolveReShadeApi (choices: Choices) (exePath: string) =
        match choices.ReShadeApi with
        | "dxgi"
        | "d3d9"
        | "opengl"
        | "vulkan" -> choices.ReShadeApi
        | _ ->
            if choices.IsEmulator || choices.Basis = Emulator then EmulatorCatalog.reShadeApi exePath
            elif choices.Basis = Dx9 then "d3d9"
            elif choices.Basis = VulkanMode then "vulkan"
            else
                // The same question route B asks, but "opengl" is its answer
                // for opengl32.dll, which the setup knows as "opengl".
                detectReShadeApi exePath

    /// The slot ReShade ends up on: DirectX 9 under the dgVoodoo wrapper moves
    /// aside to dxgi.dll, exactly as route B does it.
    let reshadeVariantFor (choices: Choices) (exePath: string) (wanted: Set<string>) =
        match resolveReShadeApi choices exePath with
        | "d3d9" when wanted.Contains partDgVoodoo -> "dxgi"
        | api -> api

    /// Every file in a payload folder, as a path relative to that folder.
    let private relativeFiles (dir: string) =
        try
            if Directory.Exists(dir) then
                Directory.GetFiles(dir, "*", SearchOption.AllDirectories)
                |> Array.map (fun f -> f.Substring(dir.Length).TrimStart('\\', '/').ToLowerInvariant())
            else
                [||]
        with _ ->
            [||]

    let private payloadNames (folders: string list) =
        lazy
            (let root = modFilesRoot ()

             if String.IsNullOrWhiteSpace(root) then
                 HashSet<string>()
             else
                 HashSet<string>(folders |> Seq.collect (fun f -> relativeFiles (Path.Combine(root, f)))))

    let private optiNames = payloadNames [ optiScalerNeuralDirName; optiScalerRtx40DirName; optiScalerDirName ]
    let private amdNames = payloadNames [ amdPayloadDirName ]
    let private dx9Names = payloadNames [ dx9PayloadDirName ]

    let private reshadeOwnNames =
        HashSet<string>(
            [ "ReShade.ini"; "ReShadePreset.ini"; "ReShade.log"; "ReShade64.json"; "ReShade64.dll"
              "ReShade32.json"; "ReShade32.dll"; "vulkan-1.dll" ],
            StringComparer.OrdinalIgnoreCase
        )

    /// The names ReShade can sit on in a record the one-click install wrote.
    let private reshadeSlotNames =
        HashSet<string>([ "dxgi.dll"; "d3d9.dll"; "d3d11.dll"; "d3d12.dll"; "opengl32.dll" ], StringComparer.OrdinalIgnoreCase)

    /// Which part a file of an untagged record belongs to, from the file alone:
    /// where it sits, what it is called, and - for the proxies, which can have
    /// any name - what its own version resource says it is.
    let private classify (mode: string) (exeDir: string) (target: string) =
        let name = Path.GetFileName(target)
        let dir = try Path.GetDirectoryName(target) with _ -> ""
        let host64Dir = Path.Combine(exeDir, host64DirName)

        let relative =
            try
                let r = Path.GetRelativePath(exeDir, target)
                if r.StartsWith("..") || Path.IsPathRooted(r) then "" else r.ToLowerInvariant()
            with _ ->
                ""

        let isDeepFried = deepFriedFileNames |> Array.exists (same name)

        if same name dlssnrFileName then
            if sameDir dir exeDir || sameDir dir host64Dir then partModel else partModelSpread
        elif relative.StartsWith(reshadeShadersDirName.ToLowerInvariant() + "\\") then
            partEffects
        elif relative.StartsWith(host64DirName.ToLowerInvariant() + "\\") then
            if isDeepFried then partDeepFried
            elif same name neuralAddonName then partNeural
            else partHost64
        elif isDeepFried then partDeepFried
        elif same name renodxAddonName || same name renodxAddonLegacyName then partRenoDx
        elif same name mfgUnlockAddonName then partMfgUnlock
        elif same name feedAddonName || same name feedAddon32Name then partFeed
        elif same name neuralAddonName then partNeural
        elif same name overlayAddonName || same name overlayAddon32Name || same name overlayConfigName then partOverlay
        elif reshadeOwnNames.Contains(name) then partReShade
        elif isReShadeFile target then partReShade
        elif isProxyOf OptiScalerProxyDll target then partOptiScaler
        elif same mode "amd" && (amdNames.Value.Contains(relative) || amdSlots |> Array.exists (same name)) then partAmd
        // Renamed from the DLL NAME card: any name, recognised by what it is.
        elif same mode "amd" && sameDir dir exeDir && name.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)
             && isAmdProxyFile target then
            partAmd
        elif optiNames.Value.Contains(relative) then partOptiScaler
        elif amdNames.Value.Contains(relative) then partAmd
        elif dx9Names.Value.Contains(relative) then partDgVoodoo
        // A proxy whose file is gone can no longer say what it was; the route
        // that wrote the record can.
        elif same mode "optiscaler" && optiScalerSlots |> Array.exists (same name) then partOptiScaler
        elif reshadeSlotNames.Contains(name) && not (same mode "optiscaler") && not (same mode "amd") then partReShade
        elif dlssFileNames |> Array.exists (same name) then partDlss
        elif name.StartsWith("sl.", StringComparison.OrdinalIgnoreCase)
             && name.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) then
            partStreamline
        else
            partExtras

    /// The part a recorded file belongs to: its tag when an advanced install
    /// wrote it, what the file itself says otherwise.
    let partOf (mode: string) (exeDir: string) (entry: InstalledFile) =
        if not (String.IsNullOrWhiteSpace(entry.Component)) then entry.Component
        else classify (if isNull mode then "" else mode) exeDir entry.TargetPath

    let private dirOf (path: string) = try Path.GetDirectoryName(path) with _ -> ""

    /// Files the user adds by hand to one game: a ReShade add-on beside its
    /// executable, an OptiScaler plugin in OptiScaler\plugins. They are the
    /// user's own, so they have no switch of their own and are never shared
    /// with the community - but they come off with the install like the rest.
    let partUserAddon = "useraddon"
    let partUserPlugin = "userplugin"

    /// A file or a whole folder of the user's own beside this one game's
    /// executable - an extra for this game alone, where Settings' extras go
    /// with every install of a route.
    let partUserExtra = "userextra"

    /// The part tags of the user's own files - none of them the mod's.
    let isUserPart (part: string) =
        part = partUserAddon || part = partUserPlugin || part = partUserExtra

    /// The first folder or file name of a path under the game's folder: what
    /// one per-game extra is, whether it is a file or a folder of files.
    let private topItem (exeDir: string) (path: string) =
        try
            let rel = Path.GetRelativePath(exeDir, path)
            let first = rel.Split([| '\\'; '/' |], StringSplitOptions.RemoveEmptyEntries)

            if first.Length = 0 || rel.StartsWith("..") then path else Path.Combine(exeDir, first.[0])
        with _ ->
            path

    /// Where OptiScaler looks for its plugins, beside the game.
    let pluginsDir (exeDir: string) = Path.Combine(exeDir, "OptiScaler", "plugins")

    /// What the copy on the game is, to hold against what is picked:
    /// ReShade's slot, RenoDX's build, OptiScaler's API, "64" / "32".
    let private variantOfPart (exeDir: string) (optiApi: string) (part: string) (entries: InstalledFile[]) =
        let inHost64 () =
            entries
            |> Array.exists (fun f -> sameDir (dirOf f.TargetPath) (Path.Combine(exeDir, host64DirName)))

        let reshadeVariant () =
            let names = entries |> Array.map (fun f -> Path.GetFileName(f.TargetPath))

            if names |> Array.exists (fun n -> same n "ReShade64.json" || same n "ReShade32.json" || same n "vulkan-1.dll") then
                "vulkan"
            else
                let slot =
                    entries
                    |> Array.tryFind (fun f -> isReShadeFile f.TargetPath)
                    |> Option.map (fun f -> Path.GetFileName(f.TargetPath).ToLowerInvariant())

                match slot with
                | Some "dxgi.dll" -> "dxgi"
                | Some "d3d9.dll" -> "d3d9"
                | Some "opengl32.dll" -> "opengl"
                // Renamed from the DLL NAME card: the user put it there on
                // purpose, so no pick here counts as a change.
                | Some _ -> "custom"
                | None -> ""

        if entries.Length = 0 then
            ""
        else
            match part with
            | p when p = partReShade -> reshadeVariant ()
            | p when p = partRenoDx ->
                if entries |> Array.exists (fun f -> same (Path.GetFileName(f.TargetPath)) renodxAddonLegacyName) then
                    "multipass"
                else
                    "standard"
            | p when p = partOptiScaler -> if same optiApi "vulkan" then "vulkan" else "dx12"
            | p when p = partFeed || p = partOverlay ->
                if entries |> Array.exists (fun f -> f.TargetPath.EndsWith(".addon32", StringComparison.OrdinalIgnoreCase)) then
                    "32"
                else
                    "64"
            | p when p = partDeepFried || p = partNeural || p = partModel -> if inHost64 () then "32" else "64"
            // The user's own files: which ones, so adding or taking one off is
            // a change the one-click install could not have made.
            | p when p = partUserAddon || p = partUserPlugin ->
                entries
                |> Array.map (fun f -> Path.GetFileName(f.TargetPath).ToLowerInvariant())
                |> Array.sort
                |> String.concat "|"
            | p when p = partUserExtra ->
                entries
                |> Array.map (fun f -> Path.GetFileName(topItem exeDir f.TargetPath).ToLowerInvariant())
                |> Array.distinct
                |> Array.sort
                |> String.concat "|"
            | _ -> ""

    /// Everything on the game in one line - "part:variant" pairs, sorted - so
    /// two installs can be compared as a whole. The one-click install a set of
    /// advanced changes started from is kept like this (`EasyBase`).
    let private signatureOf (mode: string) (optiApi: string) (exeDir: string) (files: InstalledFile[]) =
        files
        |> Array.filter (fun f -> not (isNull (box f)) && not (String.IsNullOrWhiteSpace(f.TargetPath)))
        |> Array.groupBy (partOf mode exeDir)
        |> Array.map (fun (part, entries) -> part + ":" + variantOfPart exeDir optiApi part entries)
        |> Array.sort
        |> String.concat ","

    let private filesOf (m: InstallManifest) =
        if isNull (box m.Files) then [||] else m.Files |> Array.filter (fun f -> not (isNull (box f)))

    /// A record the one-click sheet must leave alone: changes made in the
    /// advanced sheet are on the game. A record from before the flag existed
    /// (round 19) that carries part tags counts as one too.
    let private lockedRecord (m: InstallManifest) =
        same m.Advanced "1"
        || (isNull m.Advanced && filesOf m |> Array.exists (fun f -> not (String.IsNullOrWhiteSpace(f.Component))))

    /// True while this game's install holds changes made in the advanced
    /// sheet. The one-click sheet stays closed to it until they are taken off
    /// again - back to the one-click install they started from - or the whole
    /// install is removed. One small file read: fine on the UI thread.
    let isEasyLocked (game: GameItem) =
        match readManifest game with
        | Some m -> lockedRecord m
        | None -> false

    /// What an advanced run has to be able to come back to: the one-click
    /// install on the game before any advanced change, or "" when there was
    /// none (the advanced sheet made it from nothing, which stays locked
    /// until it is removed).
    let private baselineOf (game: GameItem) (exePath: string) =
        match readManifest game with
        | None -> ""
        | Some m when lockedRecord m -> if isNull m.EasyBase then "" else m.EasyBase
        | Some m ->
            let exeDir = Path.GetDirectoryName(exePath)
            signatureOf (if isNull m.Mode then "" else m.Mode) (if isNull m.Api then "" else m.Api) exeDir (filesOf m)

    /// The user's own files on the game (added through "Add addon", "Add
    /// plugin" and the per-game extras), as (part, path). A per-game extra
    /// that is a folder is one entry: the folder.
    let userFiles (game: GameItem) (exePath: string) : (string * string)[] =
        match readManifest game with
        | Some m ->
            let exeDir = try Path.GetDirectoryName(exePath) with _ -> ""

            filesOf m
            |> Array.filter (fun f -> not (isNull f.Component) && isUserPart f.Component)
            |> Array.map (fun f ->
                if f.Component = partUserExtra then (f.Component, topItem exeDir f.TargetPath)
                else (f.Component, f.TargetPath))
            |> Array.distinct
        | None -> [||]

    /// Whether the install on this game carries anything of the user's own -
    /// Settings' extras or the files added to this game by hand. The community
    /// post says EXTRA when it does.
    let hasUserExtras (game: GameItem) (exePath: string) =
        match readManifest game with
        | Some m ->
            let exeDir = try Path.GetDirectoryName(exePath) with _ -> ""
            let mode = if isNull m.Mode then "" else m.Mode

            filesOf m
            |> Array.exists (fun f ->
                not (String.IsNullOrWhiteSpace(f.TargetPath))
                && (let part = partOf mode exeDir f
                    part = partExtras || isUserPart part))
        | None -> false

    // =====================================================================
    // WHAT WOULD HAVE TO BE DONE
    // =====================================================================
    let private dlssFoldersFor (game: GameItem) (exePath: string) (plan: InstallPlan option) =
        match plan with
        | Some p -> foldersFromPaths p.DlssDirs dlssFileNames
        | None -> findDlssFolders game.InstallDirectory exePath

    let private streamlineFoldersFor (game: GameItem) (exePath: string) (plan: InstallPlan option) =
        match plan with
        | Some p -> foldersFromPaths p.StreamlineDirs streamlineFileNames
        | None -> findStreamlineFolders game.InstallDirectory exePath

    let private ourStreamlineFiles (modRoot: string) =
        let dir = Path.Combine(modRoot, "streamline_dlss", "streamline")
        if Directory.Exists(dir) then Directory.GetFiles(dir, "sl.*.dll") else [||]

    /// Route B's rule, unchanged: only a game already shipping Streamline 2.4
    /// or newer takes ours, and only when ours is newer still.
    let private streamlineQualifies (modRoot: string) (folder: ModFolder) =
        let ours = ourStreamlineFiles modRoot

        let ourVer = if ours.Length > 0 then readFileVersion ours.[0] else zeroVer
        let gameVer = folderVersion folder
        compareVer gameVer minimumStreamlineVersion >= 0 && compareVer ourVer gameVer > 0

    /// How many DLSS runtime files would be copied: upgrades in the game's
    /// own folders, and ours beside the executable where it has none as new.
    let private dlssWork (modRoot: string) (exeDir: string) (folders: ModFolder list) =
        let ourDir = Path.Combine(modRoot, "streamline_dlss", "dlss")
        let mutable n = 0

        for folder in folders do
            for f in folder.Files do
                let ours = Path.Combine(ourDir, f.Name)
                if File.Exists(ours) && compareVer f.Version (readFileVersion ours) < 0 then n <- n + 1

        if Directory.Exists(ourDir) then
            for src in Directory.GetFiles(ourDir, "*.dll") do
                let target = Path.Combine(exeDir, Path.GetFileName(src))
                if not (File.Exists(target)) || compareVer (readFileVersion target) (readFileVersion src) < 0 then n <- n + 1

        n

    /// The game's own DLSS and Streamline folders, less the one the model
    /// part already covers.
    let private spreadFolders (game: GameItem) (exePath: string) (plan: InstallPlan option) =
        let exeDir = Path.GetDirectoryName(exePath)
        let host64Dir = Path.Combine(exeDir, host64DirName)

        [ for f in dlssFoldersFor game exePath plan -> f.Directory
          for f in streamlineFoldersFor game exePath plan -> f.Directory ]
        |> List.distinctBy (fun d -> (trimDir d).ToLowerInvariant())
        |> List.filter (fun d -> Directory.Exists(d) && not (sameDir d exeDir) && not (sameDir d host64Dir))

    /// The user's extras this install would carry - the same match
    /// `deployExtras` makes.
    let private applicableExtras (choices: Choices) =
        let routeKey = modeKey choices.Basis

        let variantKey =
            match choices.Basis with
            | OptiScalerMode -> routeKey + "-" + optiApiKey choices.OptiApi
            | Dx11
            | Dx9 -> routeKey + "-" + archKey choices.Build
            | _ -> routeKey

        ExtrasStore.list ()
        |> List.filter (fun e -> e.Enabled && ExtrasStore.appliesTo e [ routeKey; variantKey ])

    /// What a part needs from "mod files", and whether it is there.
    let private blockedReason (modRoot: string) (choices: Choices) (part: string) =
        let has (relative: string) = File.Exists(Path.Combine(modRoot, relative))
        let hasDir (relative: string) = Directory.Exists(Path.Combine(modRoot, relative))
        let missingIf (cond: bool) = if cond then "" else "missing"

        if String.IsNullOrWhiteSpace(modRoot) then
            "missing"
        else
            match part with
            | p when p = partReShade -> missingIf (reShadeSetupPath () <> "")
            | p when p = partEffects -> missingIf (hasDir standardEffectsDirName)
            | p when p = partDgVoodoo -> missingIf (hasDir dx9PayloadDirName)
            | p when p = partHost64 -> missingIf (hasDir (Path.Combine(bit32PayloadDirName, host64DirName)))
            | p when p = partFeed -> missingIf (has (if choices.Build = Bit32 then feedAddon32Name else feedAddonName))
            | p when p = partRenoDx ->
                missingIf (has (if choices.RenoDxBuild = "multipass" then renodxAddonLegacyName else renodxAddonName))
            | p when p = partDeepFried ->
                missingIf (deepFriedFileNames |> Array.forall (fun n -> has (Path.Combine(emulatorPayloadDirName, n))))
            | p when p = partMfgUnlock -> missingIf (has mfgUnlockAddonName)
            | p when p = partOptiScaler -> missingIf (hasDir (optiScalerPayloadDirName choices.OptiApi))
            | p when p = partAmd -> missingIf (hasDir amdPayloadDirName)
            | p when p = partStreamline -> missingIf (hasDir (Path.Combine("streamline_dlss", "streamline")))
            | p when p = partDlss -> missingIf (hasDir (Path.Combine("streamline_dlss", "dlss")))
            | p when p = partModel || p = partModelSpread -> missingIf (has (Path.Combine(dlss5DirName, dlssnrFileName)))
            | p when p = partNeural -> missingIf (has neuralAddonName)
            | p when p = partOverlay ->
                if has (if choices.Build = Bit32 then overlayAddon32Name else overlayAddonName) then "" else "download"
            | _ -> ""

    // =====================================================================
    // READING THE GAME
    // =====================================================================
    /// Every part as it stands on the game right now. Reads version resources
    /// and may walk the game's folders when there is no scan to go on - keep
    /// it off the UI thread.
    let readState (game: GameItem) (exePath: string) (plan: InstallPlan option) (choices: Choices) : Map<string, PartState> =
        let modRoot = modFilesRoot ()
        let exeDir = try Path.GetDirectoryName(exePath) with _ -> ""

        let manifest = readManifest game

        let mode =
            match manifest with
            | Some m when not (isNull m.Mode) -> m.Mode
            | _ -> ""

        let files =
            match manifest with
            | Some m when not (isNull (box m.Files)) -> m.Files |> Array.filter (fun f -> not (isNull (box f)))
            | _ -> [||]

        let byPart =
            if String.IsNullOrWhiteSpace(exeDir) then
                Map.empty
            else
                files
                |> Array.filter (fun f -> not (String.IsNullOrWhiteSpace(f.TargetPath)))
                |> Array.groupBy (partOf mode exeDir)
                |> Map.ofArray

        let entriesOf (part: string) = byPart |> Map.tryFind part |> Option.defaultValue [||]

        let incomplete (entries: InstalledFile[]) =
            entries
            |> Array.exists (fun f ->
                not f.WasExisting
                && not (f.TargetPath.EndsWith(".log", StringComparison.OrdinalIgnoreCase))
                && not (File.Exists(f.TargetPath)))

        let recordedApi =
            match manifest with
            | Some m when not (isNull m.Api) -> m.Api
            | _ -> ""

        let variantOf (part: string) (entries: InstalledFile[]) = variantOfPart exeDir recordedApi part entries

        let dlssFolders = lazy (try dlssFoldersFor game exePath plan with _ -> [])
        let streamlineFolders = lazy (try streamlineFoldersFor game exePath plan with _ -> [])

        let idleOf (part: string) =
            try
                match part with
                | p when p = partStreamline ->
                    if streamlineFolders.Value |> List.exists (streamlineQualifies modRoot) then "" else "nostreamline"
                | p when p = partDlss -> if dlssWork modRoot exeDir dlssFolders.Value > 0 then "" else "uptodate"
                | p when p = partModelSpread -> if (spreadFolders game exePath plan).IsEmpty then "nofolders" else ""
                | p when p = partExtras -> if (applicableExtras choices).IsEmpty then "noextras" else ""
                | _ -> ""
            with _ ->
                ""

        let presentOf (part: string) =
            try
                match part with
                | p when p = partReShade ->
                    isReShadeInstalled exePath
                    || isReShadeFile (Path.Combine(exeDir, "d3d9.dll"))
                    || File.Exists(Path.Combine(exeDir, "ReShade64.json"))
                    || File.Exists(Path.Combine(exeDir, "ReShade32.json"))
                | p when p = partModel ->
                    File.Exists(Path.Combine(exeDir, dlssnrFileName))
                    || File.Exists(Path.Combine(exeDir, host64DirName, dlssnrFileName))
                | _ -> false
            with _ ->
                false

        installOrder
        |> Array.map (fun part ->
            let entries = entriesOf part
            let installed = entries.Length > 0

            part,
            { Id = part
              Files = entries |> Array.map (fun f -> f.TargetPath)
              Installed = installed
              Incomplete = installed && incomplete entries
              Present = not installed && presentOf part
              Idle = (if installed then "" else idleOf part)
              Blocked = blockedReason modRoot choices part
              Variant = variantOf part entries })
        |> Map.ofArray

    // =====================================================================
    // THE ROUTES AS PRESETS
    // =====================================================================
    type Preset =
        { Parts: Set<string>
          Build: InstallArch
          ReShadeApi: string
          OptiApi: OptiScalerApi
          RenoDxBuild: string }

    /// Exactly what the one-click install puts on the game for that route and
    /// build - the payload switches in Settings included, so a file switched
    /// off there is not ticked here either. The user can add to it.
    let recommend (mode: InstallMode) (build: InstallArch) (exePath: string) (detectedApi: string) (overlayOn: bool) : Preset =
        let on (payload: string) = ExtrasStore.isPayloadEnabled payload
        let is32 = build = Bit32 && archMatters mode

        let reshadeAlready =
            try isReShadeInstalled exePath with _ -> false

        let modelParts =
            if on dlssnrFileName then [ partModel ] else []

        let common (spread: bool) =
            [ yield partStreamline
              yield partDlss
              yield! modelParts
              if spread && on dlssnrFileName then yield partModelSpread
              yield partExtras ]

        let reshadeSide =
            [ if on reShadeSetupKey then yield partReShade
              yield partEffects ]

        let overlayPart = if overlayOn && overlaySupported mode OptiDx12 then [ partOverlay ] else []

        let parts =
            match mode with
            | OptiScalerMode ->
                [ yield partOptiScaler
                  yield! common false
                  // Route A adds the effects only to a ReShade that is already there.
                  if reshadeAlready then yield partEffects ]
            | AmdMode -> [ yield partAmd; yield! modelParts; yield partExtras ]
            | Emulator ->
                [ yield! reshadeSide
                  if on feedAddonName then yield partFeed
                  yield partDlss
                  yield partDeepFried
                  yield! modelParts
                  yield! overlayPart
                  yield partExtras ]
            | VulkanMode ->
                [ yield! reshadeSide
                  // No RenoDX on Vulkan: the payload is the effect itself.
                  yield partDeepFried
                  if on feedAddonName then yield partFeed
                  yield! common true
                  yield! overlayPart ]
            | Dx12Auto
            | Dx11
            | Dx9 ->
                [ yield! reshadeSide
                  if mode = Dx9 then yield partDgVoodoo

                  if is32 then
                      if on feedAddon32Name then yield partFeed
                      yield partHost64
                      // The point of the 32-bit route, so it arrives ticked.
                      yield partDeepFried
                  else
                      if on renodxAddonName then yield partRenoDx
                      if on feedAddonName then yield partFeed

                  yield! common true
                  yield! overlayPart ]

        { Parts = Set.ofList parts
          Build = (if is32 then Bit32 else Bit64)
          ReShadeApi = "auto"
          OptiApi = (if mode = OptiScalerMode && same detectedApi "vulkan" then OptiVulkan else OptiDx12)
          // The one-click sheet's Multipass arrives on (round 26): its build
          // is what the route installs now.
          RenoDxBuild = "multipass" }

    // =====================================================================
    // THE RESTORE POINT
    // =====================================================================
    /// `baseline` is the one-click install these changes started from (see
    /// `baselineOf`). Back on it exactly, the record is a one-click one again
    /// and the one-click sheet opens to the game; anywhere else it is locked.
    let private writeRecord (game: GameItem) (exePath: string) (choices: Choices) (baseline: string) (files: InstalledFile[]) =
        let exeDir = Path.GetDirectoryName(exePath)
        let sidecar = Path.Combine(exeDir, installRecordName)

        if files.Length = 0 then
            // Nothing of ours is left: no record, exactly as after a removal.
            try File.Delete(manifestPath game) with _ -> ()
            try if File.Exists(sidecar) then File.Delete(sidecar) with _ -> ()
        else
            let mode = modeKey choices.Basis
            let parts = files |> Array.map (partOf mode exeDir) |> Set.ofArray
            let flag (part: string) = if parts.Contains part then "1" else ""
            let backToEasy = baseline <> "" && signatureOf mode (optiApiKey choices.OptiApi) exeDir files = baseline

            let multipass =
                files
                |> Array.exists (fun f -> same (Path.GetFileName(f.TargetPath)) renodxAddonLegacyName)

            let manifest =
                { GameId = safeId game
                  GameTitle = game.Title
                  ExecutablePath = exePath
                  InstalledAtUtc = DateTime.UtcNow.ToString("o")
                  Mode = mode
                  Arch = archKey choices.Build
                  Api = optiApiKey choices.OptiApi
                  Neural = flag partNeural
                  MfgUnlock = flag partMfgUnlock
                  Multipass = (if multipass then "1" else "")
                  DeepFried = flag partDeepFried
                  EmuApi =
                    (if choices.Basis = Emulator then
                         (if resolveReShadeApi choices exePath = "vulkan" then "vulkan" else "dx12")
                     else
                         "")
                  Advanced = (if backToEasy then "0" else "1")
                  EasyBase = (if backToEasy || baseline = "" then null else baseline)
                  Files = files }

            let options = JsonSerializerOptions()
            options.WriteIndented <- true
            let json = JsonSerializer.Serialize(manifest, options)
            File.WriteAllText(manifestPath game, json)

            try
                File.WriteAllText(sidecar, json)
            with _ ->
                ()

    // =====================================================================
    // TAKING PARTS OFF
    // =====================================================================
    /// Files the mod writes by itself once the game has run, by part - the
    /// restore point is written before that and cannot know them.
    let private leftoversOf (part: string) =
        match part with
        | p when p = partReShade -> [ "ReShade.log" ]
        | p when p = partOptiScaler -> [ "OptiScaler.log" ]
        | p when p = partAmd -> [ "dlssnr_on_amd.log" ]
        | p when p = partFeed -> [ "dlss5-feed.cfg" ]
        | p when p = partDgVoodoo -> [ "dgVoodoo.log" ]
        | p when p = partOverlay -> [ "dlss5-overlay.ini.bak"; "dlss5-overlay.log" ]
        | _ -> []

    let private tryDelete (path: string) =
        try
            if File.Exists(path) then
                File.Delete(path)
                true
            else
                false
        with _ ->
            false

    /// The payload folders, dropped once nothing of anyone's is left inside.
    let private dropEmptyFolders (exeDir: string) =
        for name in [ "OptiScaler"; "Licenses"; reshadeShadersDirName; host64DirName ] do
            try
                let dir = Path.Combine(exeDir, name)

                if Directory.Exists(dir) && Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Length = 0 then
                    Directory.Delete(dir, true)
            with _ ->
                ()

    /// Puts back what those parts replaced and deletes what they added, then
    /// rewrites the restore point without them. Returns (restored, removed).
    let private removeParts
        (game: GameItem)
        (exePath: string)
        (choices: Choices)
        (baseline: string)
        (parts: Set<string>)
        (report: Progress)
        (fromP: float)
        (span: float)
        =
        match readManifest game with
        | None -> (0, 0)
        | Some m ->
            let exeDir = Path.GetDirectoryName(exePath)
            let mode = if isNull m.Mode then "" else m.Mode
            let files = if isNull (box m.Files) then [||] else m.Files |> Array.filter (fun f -> not (isNull (box f)))

            let taking, keeping =
                files
                |> Array.partition (fun f ->
                    not (String.IsNullOrWhiteSpace(f.TargetPath)) && parts.Contains(partOf mode exeDir f))

            let mutable restored = 0
            let mutable removed = 0
            let total = max 1 taking.Length
            let mutable index = 0

            for entry in Array.rev taking do
                index <- index + 1
                report "Removing the parts you switched off..." (fromP + span * 0.8 * float index / float total)

                try
                    if entry.WasExisting && File.Exists(entry.BackupPath) then
                        File.Copy(entry.BackupPath, entry.TargetPath, true)
                        restored <- restored + 1
                    elif not entry.WasExisting && File.Exists(entry.TargetPath) then
                        File.Delete(entry.TargetPath)
                        removed <- removed + 1
                with _ ->
                    ()

            for part in parts do
                for name in leftoversOf part do
                    for dir in [ exeDir; Path.Combine(exeDir, host64DirName) ] do
                        if tryDelete (Path.Combine(dir, name)) then removed <- removed + 1

                if part = partOptiScaler then
                    // Anything still carrying OptiScaler's version resource is
                    // ours, whatever name it ended up under - as on removal.
                    removed <- removed + clearStaleOptiScalerHooks exeDir

                if part = partOverlay then
                    for name in overlayWrittenEffects do
                        for dir in [ Path.Combine(exeDir, reshadeShadersDirName, "Shaders")
                                     Path.Combine(exeDir, reshadeShadersDirName)
                                     exeDir ] do
                            if tryDelete (Path.Combine(dir, name)) then removed <- removed + 1

            dropEmptyFolders exeDir
            report "Updating the restore point..." (fromP + span * 0.95)
            writeRecord game exePath choices baseline keeping
            (restored, removed)

    // =====================================================================
    // PUTTING PARTS ON
    // =====================================================================
    /// ReShade.ini's search paths, pointed at the effects folder - the half of
    /// `ensureStandardEffectPaths` that edits the ini, for a ReShade that
    /// arrives after the effects did.
    let private writeEffectPaths (exeDir: string) =
        let ini = Path.Combine(exeDir, "ReShade.ini")

        if File.Exists(ini) then
            try
                let wanted =
                    [ "EffectSearchPaths", ".\\" + reshadeShadersDirName + "\\Shaders\\**"
                      "TextureSearchPaths", ".\\" + reshadeShadersDirName + "\\Textures\\**" ]

                let startsWithKey (key: string) (line: string) =
                    line.TrimStart().StartsWith(key + "=", StringComparison.OrdinalIgnoreCase)

                let updated =
                    wanted
                    |> List.fold
                        (fun (lines: string list) (key, value) ->
                            if lines |> List.exists (startsWithKey key) then
                                lines |> List.map (fun l -> if startsWithKey key l then key + "=" + value else l)
                            else
                                match lines |> List.tryFindIndex (fun l -> l.Trim().Equals("[GENERAL]", StringComparison.OrdinalIgnoreCase)) with
                                | Some i -> List.truncate (i + 1) lines @ [ key + "=" + value ] @ List.skip (i + 1) lines
                                | None -> [ "[GENERAL]"; key + "=" + value ] @ lines)
                        (File.ReadAllLines(ini) |> List.ofArray)

                File.WriteAllLines(ini, updated)
            with _ ->
                ()

    /// Our own OptiScaler proxy on the slot ReShade is about to take moves to
    /// the next free name first, through the DLL NAME card's own rename - the
    /// two cannot share one file, and the ReShade setup would write over it.
    let private freeSlotForReShade (game: GameItem) (exeDir: string) (slot: string) =
        if slot <> "" then
            let path = Path.Combine(exeDir, slot)

            if isProxyOf OptiScalerProxyDll path && same (installedProxyPath game OptiScalerProxyDll) path then
                match optiScalerProxyNames |> Array.tryFind (fun n -> not (same n slot) && not (File.Exists(Path.Combine(exeDir, n)))) with
                | Some free -> renameProxy game OptiScalerProxyDll free |> ignore
                | None -> ()

    /// Moves ReShade off d3d9.dll onto dxgi.dll so the dgVoodoo wrapper can
    /// have the d3d9 slot - route B's step 1a. The game's own dxgi.dll, if it
    /// has one, is backed up first and comes back on removal.
    let private moveReShadeToDxgi (tracker: Tracker) (game: GameItem) (exeDir: string) =
        let d3d9 = Path.Combine(exeDir, "d3d9.dll")
        let dxgi = Path.Combine(exeDir, "dxgi.dll")

        if isReShadeFile d3d9 && not (isReShadeFile dxgi) then
            let existed = File.Exists(dxgi)

            let backup =
                if existed then
                    let bp = Path.Combine(backupRoot game, shortHash dxgi + "_dxgi.dll")
                    if not (File.Exists(bp)) then File.Copy(dxgi, bp, false)
                    bp
                else
                    ""

            File.Move(d3d9, dxgi, true)
            let previous = tracker.Component
            tracker.Component <- partReShade
            tracker.Record(dxgi, existed, backup)
            tracker.Component <- previous
            // d3d9.dll is the wrapper's now: whatever the record said about
            // that name - an original the game shipped, or nothing - still
            // holds, and it comes off with the wrapper.
            tracker.Retag(d3d9, partDgVoodoo)
            true
        else
            false

    let private installParts
        (game: GameItem)
        (exePath: string)
        (plan: InstallPlan option)
        (choices: Choices)
        (baseline: string)
        (wanted: Set<string>)
        (parts: Set<string>)
        (report: Progress)
        (fromP: float)
        (span: float)
        : string list =

        let modRoot = modFilesRoot ()
        let exeDir = Path.GetDirectoryName(exePath)
        let is32 = (choices.Build = Bit32)
        let host64Target = Path.Combine(exeDir, host64DirName)
        let primaryDir = if is32 then host64Target else exeDir
        let lines = List<string>()
        let has (p: string) = parts.Contains p

        let reshadeApi = resolveReShadeApi choices exePath
        let reshadeSlot = reshadeSlotFile reshadeApi

        if has partReShade then freeSlotForReShade game exeDir reshadeSlot

        let prior =
            match readManifest game with
            | Some m when not (isNull (box m.Files)) -> m.Files |> Array.filter (fun f -> not (isNull (box f)))
            | _ -> [||]

        let tracker = Tracker(backupRoot game, prior)

        // One slice of the bar per part, in the order they go in.
        let order = installOrder |> Array.filter has
        let slice (part: string) =
            let i = max 0 (Array.IndexOf(order, part))
            let w = span / float (max 1 order.Length)
            (fromP + w * float i, w)

        let step (part: string) (text: string) =
            tracker.Component <- part
            let (start, _) = slice part
            report text start

        let copyTo (source: string) (target: string) =
            Directory.CreateDirectory(Path.GetDirectoryName(target)) |> ignore
            tracker.Copy(source, target)

        try
            // ---- ReShade -------------------------------------------------
            if has partReShade then
                step partReShade (sprintf "Installing ReShade runtime (%s)..." reshadeApi)
                let (start, width) = slice partReShade
                let slotPath = if reshadeSlot = "" then "" else Path.Combine(exeDir, reshadeSlot)
                let underWrapper = reshadeApi = "d3d9" && wanted.Contains partDgVoodoo

                let already =
                    isReShadeInstalled exePath
                    || (slotPath <> "" && isReShadeFile slotPath)
                    || isReShadeFile (Path.Combine(exeDir, "d3d9.dll"))
                    || prior |> Array.exists (fun f -> f.TargetPath.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) && isProxyOf ReShadeProxyDll f.TargetPath)

                if already then
                    lines.Add "ReShade already in the game - kept as it is"
                else
                    let setupExe = reShadeSetupPath ()

                    if setupExe = "" then
                        failwith "The ReShade setup is missing from \"mod files\"."

                    let artifacts =
                        [ "vulkan-1.dll"; "ReShade64.json"; "ReShade64.dll"; "ReShade32.json"; "ReShade32.dll"
                          "ReShade.ini"; "ReShadePreset.ini"; "ReShade.log" ]
                        |> List.filter (fun n -> not (same n reshadeSlot))

                    let before = artifacts |> List.filter (fun n -> File.Exists(Path.Combine(exeDir, n))) |> Set.ofList

                    // Under the wrapper, whatever holds d3d9.dll now - the
                    // wrapper itself, from an earlier run - steps aside while
                    // the setup writes its own d3d9.dll, and comes back after.
                    let aside =
                        if underWrapper && File.Exists(slotPath) then
                            let tmp = Path.Combine(backupRoot game, "aside_" + Guid.NewGuid().ToString("N") + "_d3d9.dll")
                            File.Move(slotPath, tmp)
                            tmp
                        else
                            ""

                    // Anywhere else the game's own file on ReShade's slot is
                    // backed up once and handed back on removal - route B.
                    let slotExisted = not underWrapper && slotPath <> "" && File.Exists(slotPath)

                    let slotBackup =
                        if slotExisted then
                            let bp = Path.Combine(backupRoot game, shortHash slotPath + "_" + reshadeSlot)
                            if not (File.Exists(bp)) then File.Copy(slotPath, bp, false)
                            bp
                        else
                            ""

                    let setupReport: Progress =
                        fun text p -> report text (start + width * 0.9 * (max 0.0 (min 1.0 ((p - 0.10) / 0.08))))

                    let (ok, err) = runReShadeSetup setupExe exePath reshadeApi setupReport

                    let slotNow = slotPath <> "" && File.Exists(slotPath)
                    let layerNow = File.Exists(Path.Combine(exeDir, "ReShade64.json")) || File.Exists(Path.Combine(exeDir, "ReShade32.json"))

                    if not ok && not slotNow && not layerNow && not (isReShadeInstalled exePath) then
                        if aside <> "" then (try File.Move(aside, slotPath, true) with _ -> ())
                        failwith (if String.IsNullOrWhiteSpace(err) then "ReShade setup failed." else err)

                    if underWrapper then
                        // ReShade's own d3d9.dll goes to dxgi.dll, and the
                        // wrapper (if it was here) goes back where it was.
                        if slotNow then
                            let dxgi = Path.Combine(exeDir, "dxgi.dll")
                            let existed = File.Exists(dxgi)

                            let backup =
                                if existed && not (isReShadeFile dxgi) then
                                    let bp = Path.Combine(backupRoot game, shortHash dxgi + "_dxgi.dll")
                                    if not (File.Exists(bp)) then File.Copy(dxgi, bp, false)
                                    bp
                                else
                                    ""

                            report "Moving ReShade to the dxgi slot..." (start + width * 0.92)
                            File.Move(slotPath, dxgi, true)
                            tracker.Record(dxgi, existed, backup)

                        if aside <> "" then File.Move(aside, slotPath, true)
                    elif slotNow then
                        tracker.Record(slotPath, slotExisted, slotBackup)

                    for name in artifacts do
                        let p = Path.Combine(exeDir, name)
                        if File.Exists(p) && not (before.Contains name) then tracker.Record(p, false, "")

                    // Effects that were already here are pointed at again - a
                    // fresh ReShade.ini knows nothing of them.
                    if Directory.Exists(Path.Combine(exeDir, reshadeShadersDirName)) then writeEffectPaths exeDir

                    lines.Add(
                        sprintf
                            "ReShade installed (%s)"
                            (if underWrapper then "dxgi.dll, under dgVoodoo"
                             elif reshadeSlot = "" then "Vulkan layer"
                             else reshadeSlot)
                    )

            // ---- Standard effects ---------------------------------------
            if has partEffects then
                step partEffects "Installing standard effects (DisplayDepth, UIMask, ...)..."
                let shadersRoot = Path.Combine(exeDir, reshadeShadersDirName)
                Directory.CreateDirectory(Path.Combine(shadersRoot, "Shaders")) |> ignore
                Directory.CreateDirectory(Path.Combine(shadersRoot, "Textures")) |> ignore
                let n = copyTree tracker (Path.Combine(modRoot, standardEffectsDirName)) shadersRoot
                // The ini half only where ReShade has one: an ini written for
                // a ReShade that is not there would be litter.
                writeEffectPaths exeDir
                lines.Add(sprintf "Effects deployed (%d file(s))" n)

            // ---- DirectX 9 wrapper --------------------------------------
            if has partDgVoodoo then
                step partDgVoodoo "Installing the DirectX 9 wrapper..."

                if moveReShadeToDxgi tracker game exeDir then
                    lines.Add "ReShade moved to dxgi.dll for the wrapper"

                tracker.Component <- partDgVoodoo
                let n = copyTree tracker (Path.Combine(modRoot, dx9PayloadDirName)) exeDir
                lines.Add(sprintf "DirectX 9 wrapper deployed (%d file(s))" n)

            // ---- 64-bit host --------------------------------------------
            if has partHost64 then
                step partHost64 "Installing the 64-bit host..."
                let source = Path.Combine(modRoot, bit32PayloadDirName, host64DirName)
                // Route B's own exception: the 64-bit feed add-on only
                // registers inside the helper and does nothing there.
                let copied = copyTreeExcept tracker source host64Target [| feedAddonName |]
                let ourDlssDir = Path.Combine(modRoot, "streamline_dlss", "dlss")
                let mutable extra = 0

                for name in [ "nvngx_dlss.dll"; "nvngx_dlssg.dll" ] do
                    let src = Path.Combine(ourDlssDir, name)

                    if File.Exists(src) then
                        copyTo src (Path.Combine(host64Target, name))
                        extra <- extra + 1

                lines.Add(sprintf "64-bit host deployed (%d file(s))" (copied + extra))

            // ---- DLSS 5 feed --------------------------------------------
            if has partFeed then
                step partFeed "Installing the DLSS 5 add-on..."
                let name = if is32 then feedAddon32Name else feedAddonName
                copyTo (Path.Combine(modRoot, name)) (Path.Combine(exeDir, name))
                lines.Add(name + " deployed")

            // ---- RenoDX -------------------------------------------------
            if has partRenoDx then
                step partRenoDx "Installing RenoDX DLSS 5 add-on..."
                let name = if choices.RenoDxBuild = "multipass" then renodxAddonLegacyName else renodxAddonName
                copyTo (Path.Combine(modRoot, name)) (Path.Combine(exeDir, name))
                lines.Add(if choices.RenoDxBuild = "multipass" then "Multipass build of RenoDX deployed" else "RenoDX deployed")

            // ---- Deep-fried chicken -------------------------------------
            if has partDeepFried then
                step partDeepFried "Installing the deep-fried chicken payload..."
                // Beside a 64-bit game; inside host64 for a 32-bit one, which
                // cannot load a single 64-bit module - route B's two places.
                for n in deepFriedFileNames do
                    let src = Path.Combine(modRoot, emulatorPayloadDirName, n)
                    if File.Exists(src) then copyTo src (Path.Combine(primaryDir, n))

                lines.Add(if is32 then "Deep-fried chicken deployed inside host64" else "Deep-fried chicken deployed")

            // ---- MFG unlock ---------------------------------------------
            if has partMfgUnlock then
                step partMfgUnlock "Installing the RenoDX MFG unlock add-on..."
                copyTo (Path.Combine(modRoot, mfgUnlockAddonName)) (Path.Combine(exeDir, mfgUnlockAddonName))
                lines.Add "RenoDX MFG unlock add-on deployed"

            // ---- OptiScaler ---------------------------------------------
            if has partOptiScaler then
                step partOptiScaler "Clearing previous OptiScaler hooks..."
                let optiDirName = optiScalerPayloadDirName choices.OptiApi
                let optiRoot = Path.Combine(modRoot, optiDirName)

                if not (Directory.Exists(optiRoot)) then
                    failwith ("The \"" + optiDirName + "\" payload is missing from \"mod files\".")

                // Route A, step for step: stale hooks first, then a renamed
                // one from an earlier install, then the first free slot.
                clearStaleOptiScalerHooks exeDir |> ignore

                for f in prior do
                    if not (String.IsNullOrWhiteSpace(f.TargetPath))
                       && f.TargetPath.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)
                       && isProxyOf OptiScalerProxyDll f.TargetPath then
                        try
                            if f.WasExisting && File.Exists(f.BackupPath) then File.Copy(f.BackupPath, f.TargetPath, true)
                            else File.Delete(f.TargetPath)
                        with _ ->
                            ()

                let slotName = pickOptiScalerSlot exeDir (choices.OptiApi = OptiVulkan)

                let deployed =
                    copyTreeExcept
                        tracker
                        optiRoot
                        exeDir
                        [| dlssnrFileName; "OptiScaler.dll"; "setup_windows.bat"; "setup_linux.sh"
                           "README.md"; "INSTALL-DLSSNR.md"; "get_streamline.ps1"
                           "docs\\"; "redist\\"; "images\\"; "tests\\"; "OptiScaler\\dlssnr\\" |]

                report (sprintf "Hooking OptiScaler as %s..." slotName) (fst (slice partOptiScaler))
                tracker.Copy(Path.Combine(optiRoot, "OptiScaler.dll"), Path.Combine(exeDir, slotName))
                lines.Add(sprintf "OptiScaler deployed (%d file(s)), hooked as %s" (deployed + 1) slotName)

            // ---- AMD RDNA 4 ---------------------------------------------
            if has partAmd then
                step partAmd "Installing the AMD payload..."
                let amdPayload = Path.Combine(modRoot, amdPayloadDirName)

                let slotName =
                    amdSlots
                    |> Array.tryFind (fun n -> not (File.Exists(Path.Combine(exeDir, n))))
                    |> Option.defaultValue amdSlots.[0]

                let deployed = copyTreeExcept tracker amdPayload exeDir [| "version.dll" |]
                tracker.Copy(Path.Combine(amdPayload, "version.dll"), Path.Combine(exeDir, slotName))
                lines.Add(sprintf "AMD payload deployed (%d file(s)), hooked as %s" (deployed + 1) slotName)

            // ---- Streamline ---------------------------------------------
            if has partStreamline then
                step partStreamline "Scanning for NVIDIA Streamline..."
                let ours = ourStreamlineFiles modRoot
                let mutable updated = 0

                for folder in streamlineFoldersFor game exePath plan do
                    if streamlineQualifies modRoot folder then
                        for src in ours do
                            tracker.Copy(src, Path.Combine(folder.Directory, Path.GetFileName(src)))

                        updated <- updated + 1

                lines.Add(
                    if updated > 0 then sprintf "Streamline updated in %d folder(s)" updated
                    else "Streamline left untouched (none, or not new enough to take ours)"
                )

            // ---- DLSS runtime -------------------------------------------
            if has partDlss then
                step partDlss "Deploying DLSS runtime..."
                let ourDir = Path.Combine(modRoot, "streamline_dlss", "dlss")
                let mutable n = 0

                for folder in dlssFoldersFor game exePath plan do
                    for f in folder.Files do
                        let ours = Path.Combine(ourDir, f.Name)

                        if File.Exists(ours) && compareVer f.Version (readFileVersion ours) < 0 then
                            tracker.Copy(ours, f.Path)
                            n <- n + 1

                if Directory.Exists(ourDir) then
                    for src in Directory.GetFiles(ourDir, "*.dll") do
                        let target = Path.Combine(exeDir, Path.GetFileName(src))

                        if not (File.Exists(target)) || compareVer (readFileVersion target) (readFileVersion src) < 0 then
                            tracker.Copy(src, target)
                            n <- n + 1

                lines.Add(if n > 0 then sprintf "DLSS runtime deployed (%d file(s))" n else "DLSS runtime already up to date")

            // ---- The model ----------------------------------------------
            let dlssnrFile = Path.Combine(modRoot, dlss5DirName, dlssnrFileName)

            if has partModel then
                step partModel "Deploying DLSS 5 ray reconstruction model (165 MB)..."
                copyTo dlssnrFile (Path.Combine(primaryDir, dlssnrFileName))
                lines.Add(if is32 then "Ray reconstruction model deployed inside host64" else "Ray reconstruction model deployed next to the game")

            if has partModelSpread then
                step partModelSpread "Deploying the model to the game's DLSS folders..."
                let targets = spreadFolders game exePath plan

                for dir in targets do
                    copyTo dlssnrFile (Path.Combine(dir, dlssnrFileName))

                lines.Add(sprintf "Ray reconstruction model deployed to %d more folder(s)" targets.Length)

            // ---- Neural upstream ----------------------------------------
            if has partNeural then
                step partNeural "Installing the neural upstream add-on..."
                copyTo (Path.Combine(modRoot, neuralAddonName)) (Path.Combine(primaryDir, neuralAddonName))
                lines.Add "Neural upstream add-on deployed"

            // ---- Overlay ------------------------------------------------
            if has partOverlay then
                step partOverlay "Installing the in-game overlay..."

                if deployOverlay tracker exeDir modRoot is32 { choices.Overlay with Enabled = true } report (fst (slice partOverlay)) then
                    lines.Add "Overlay installed"
                else
                    lines.Add "Overlay skipped - download it first"

            // ---- The user's own extras ----------------------------------
            if has partExtras then
                step partExtras "Installing your extras..."
                let n = deployExtras tracker exeDir choices.Basis choices.Build choices.OptiApi report (fst (slice partExtras))
                lines.Add(if n > 0 then sprintf "%d extra file(s) deployed" n else "No extras for this route")

            tracker.Component <- null
            report "Writing restore point..." (fromP + span)
            writeRecord game exePath choices baseline tracker.Entries
            List.ofSeq lines
        with ex ->
            // Undo only what this run added, as the one-click install does, and
            // keep the record true to what is left on the disk.
            for entry in Seq.rev tracker.AddedThisRun do
                try
                    if entry.WasExisting && File.Exists(entry.BackupPath) then
                        File.Copy(entry.BackupPath, entry.TargetPath, true)
                    elif not entry.WasExisting && File.Exists(entry.TargetPath) then
                        File.Delete(entry.TargetPath)
                with _ ->
                    ()

            let added = HashSet<string>(tracker.AddedThisRun |> Seq.map (fun e -> e.TargetPath), StringComparer.OrdinalIgnoreCase)
            let kept = tracker.Entries |> Array.filter (fun e -> not (added.Contains(e.TargetPath)))

            try
                writeRecord game exePath choices baseline kept
            with _ ->
                ()

            raise ex

    // =====================================================================
    // ONE RUN
    // =====================================================================
    /// Takes `remove` off, then puts `install` on - a part picked in another
    /// build or variant is in both, and comes back the new way. `wanted` is
    /// everything that should be on the game afterwards.
    let apply
        (game: GameItem)
        (exePath: string)
        (plan: InstallPlan option)
        (choices: Choices)
        (wanted: Set<string>)
        (install: Set<string>)
        (remove: Set<string>)
        (report: Progress)
        : InstallOutcome =
        try
            report "Preparing..." 0.02

            if String.IsNullOrWhiteSpace(exePath) || not (File.Exists(exePath)) then
                { Success = false
                  Message = "Game executable was not found. Pick it manually and try again." }
            elif String.IsNullOrWhiteSpace(modFilesRoot ()) then
                { Success = false
                  Message = "The \"mod files\" folder is missing next to the application." }
            elif not (canWriteTo (Path.GetDirectoryName(exePath))) then
                { Success = false; Message = elevationNeededMessage }
            else
                // Read before anything moves: the one-click install this run
                // may take the game away from, or back to.
                let baseline = baselineOf game exePath

                // The user's own add-ons and plugins live and die with what
                // loads them: ReShade taken off takes its add-ons, OptiScaler
                // its plugins. Not when it is only put back another way.
                let remove =
                    remove
                    |> (fun r ->
                        if r.Contains partReShade && not (install.Contains partReShade) then r.Add partUserAddon else r)
                    |> (fun r ->
                        if r.Contains partOptiScaler && not (install.Contains partOptiScaler) then r.Add partUserPlugin
                        else r)

                let removing = not remove.IsEmpty
                let installing = not install.IsEmpty

                let (restored, removed) =
                    if removing then
                        removeParts game exePath choices baseline remove report 0.04 (if installing then 0.2 else 0.9)
                    else
                        (0, 0)

                let lines =
                    if installing then
                        installParts game exePath plan choices baseline wanted install report (if removing then 0.26 else 0.06) (if removing then 0.7 else 0.9)
                    else
                        []

                report "Done." 1.0

                // The parts the user switched off - their own files going along
                // with ReShade or OptiScaler are not a part of their own.
                let partsOff =
                    (remove - install) |> Set.filter (fun p -> p <> partUserAddon && p <> partUserPlugin)

                let headline =
                    match installing, removing with
                    | true, true when not partsOff.IsEmpty ->
                        sprintf "Advanced install: %d part(s) installed, %d taken off" install.Count partsOff.Count
                    | true, _ -> sprintf "Advanced install: %d part(s) installed" install.Count
                    | false, true -> sprintf "Advanced install: %d part(s) taken off" partsOff.Count
                    | false, false -> "Advanced install: nothing to change"

                let removalLine =
                    if removing then
                        [ sprintf "Restored %d original file(s) and removed %d added file(s)" restored removed ]
                    else
                        []

                { Success = true
                  Message = String.Join(" • ", headline :: (removalLine @ lines)) }
        with ex ->
            { Success = false
              Message = "The advanced install stopped and was rolled back: " + ex.Message }

    // =====================================================================
    // THE USER'S OWN FILES - "Add addon" under ReShade, "Add plugin" under
    // OptiScaler. For the one game whose sheet is open, and nothing more.
    // =====================================================================
    let private modeOfKey (key: string) =
        match key with
        | "optiscaler" -> OptiScalerMode
        | "dx11" -> Dx11
        | "dx9" -> Dx9
        | "vulkan" -> VulkanMode
        | "emulator" -> Emulator
        | "amd" -> AmdMode
        | _ -> Dx12Auto

    /// The record's own route, build and API: a file added by hand changes
    /// what is on the game, not how the game was installed.
    let private choicesOfRecord (m: InstallManifest) (fallback: Choices) : Choices =
        { fallback with
            Build = (if m.Arch = "32" then Bit32 else Bit64)
            OptiApi = (if same m.Api "vulkan" then OptiVulkan else OptiDx12)
            Basis = modeOfKey (if isNull m.Mode then "" else m.Mode) }

    /// Copies one file of the user's own onto this game: a ReShade add-on
    /// beside the executable (`partUserAddon`), or an OptiScaler plugin into
    /// OptiScaler\plugins (`partUserPlugin`). A file of the game's by that
    /// name is backed up first and handed back when this one comes off; a file
    /// the mod itself put there is never overwritten.
    let addUserFile (game: GameItem) (exePath: string) (choices: Choices) (part: string) (source: string) : InstallOutcome =
        try
            if String.IsNullOrWhiteSpace(exePath) || not (File.Exists(exePath)) then
                { Success = false
                  Message = "Game executable was not found. Pick it manually and try again." }
            elif String.IsNullOrWhiteSpace(source) || not (File.Exists(source)) then
                { Success = false; Message = "That file cannot be read." }
            elif not (canWriteTo (Path.GetDirectoryName(exePath))) then
                { Success = false; Message = elevationNeededMessage }
            else
                let exeDir = Path.GetDirectoryName(exePath)
                let targetDir = if part = partUserPlugin then pluginsDir exeDir else exeDir
                let target = Path.Combine(targetDir, Path.GetFileName(source))
                let exists (path: string) = File.Exists(path) || Directory.Exists(path)

                if same (Path.GetFullPath(source)) (Path.GetFullPath(target)) then
                    { Success = false; Message = "That file is already there." }
                else
                    let manifest = readManifest game
                    let prior = match manifest with Some m -> filesOf m | None -> [||]

                    match prior |> Array.tryFind (fun f -> same f.TargetPath target) with
                    | Some f when not (same f.Component part) ->
                        { Success = false
                          Message = Path.GetFileName(target) + " is already a file of the mod here - rename yours first." }
                    // A per-game extra never stands in for a file of the game's:
                    // a game file by that name is left alone and the extra refused.
                    | None when part = partUserExtra && exists target ->
                        { Success = false
                          Message = Path.GetFileName(target) + " is already in the game folder - rename yours first." }
                    | _ ->
                        let baseline = baselineOf game exePath

                        let recordChoices =
                            match manifest with
                            | Some m -> choicesOfRecord m choices
                            | None -> choices

                        let tracker = Tracker(backupRoot game, prior)
                        tracker.Component <- part
                        Directory.CreateDirectory(targetDir) |> ignore
                        tracker.Copy(source, target)
                        writeRecord game exePath recordChoices baseline tracker.Entries

                        { Success = true
                          Message =
                            sprintf
                                "%s added to %s"
                                (Path.GetFileName(target))
                                (if part = partUserPlugin then "OptiScaler\\plugins" else "the game folder") }
        with ex ->
            { Success = false; Message = "Could not add the file: " + ex.Message }

    /// A folder the user's extra brought, once nothing is left in it - its
    /// empty subfolders first. Never a folder that still holds a file.
    let private dropEmptyTree (path: string) =
        try
            if Directory.Exists(path) then
                for d in Directory.GetDirectories(path, "*", SearchOption.AllDirectories) |> Array.sortByDescending (fun d -> d.Length) do
                    if Directory.GetFileSystemEntries(d).Length = 0 then Directory.Delete(d)

                if Directory.GetFileSystemEntries(path).Length = 0 then Directory.Delete(path)
        with _ ->
            ()

    /// A folder of the user's own, copied whole beside this game's executable
    /// under its own name - an extra for this game alone. A folder of that
    /// name already in the game is refused rather than merged into.
    let addUserFolder (game: GameItem) (exePath: string) (choices: Choices) (source: string) : InstallOutcome =
        try
            if String.IsNullOrWhiteSpace(exePath) || not (File.Exists(exePath)) then
                { Success = false
                  Message = "Game executable was not found. Pick it manually and try again." }
            elif String.IsNullOrWhiteSpace(source) || not (Directory.Exists(source)) then
                { Success = false; Message = "That folder cannot be read." }
            elif not (canWriteTo (Path.GetDirectoryName(exePath))) then
                { Success = false; Message = elevationNeededMessage }
            else
                let exeDir = Path.GetDirectoryName(exePath)
                let name = Path.GetFileName(source.TrimEnd('\\', '/'))
                let target = Path.Combine(exeDir, name)

                if String.IsNullOrWhiteSpace(name) then
                    { Success = false; Message = "That folder cannot be read." }
                elif Directory.Exists(target) || File.Exists(target) then
                    { Success = false
                      Message = name + " is already in the game folder - rename yours first." }
                elif exeDir.StartsWith(Path.GetFullPath(source).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase) then
                    { Success = false; Message = "That folder holds the game itself." }
                else
                    let manifest = readManifest game
                    let prior = match manifest with Some m -> filesOf m | None -> [||]
                    let baseline = baselineOf game exePath

                    let recordChoices =
                        match manifest with
                        | Some m -> choicesOfRecord m choices
                        | None -> choices

                    let tracker = Tracker(backupRoot game, prior)
                    tracker.Component <- partUserExtra
                    let n = copyTree tracker source target
                    writeRecord game exePath recordChoices baseline tracker.Entries
                    { Success = true; Message = sprintf "%s added to the game folder (%d file(s))" name n }
        with ex ->
            { Success = false; Message = "Could not add the folder: " + ex.Message }

    /// Takes one of the user's own files off again - and puts back whatever
    /// of the game's it had replaced.
    let removeUserFile (game: GameItem) (exePath: string) (choices: Choices) (target: string) : InstallOutcome =
        try
            match readManifest game with
            | None -> { Success = false; Message = "This game has no install record." }
            | Some m ->
                let files = filesOf m

                let under (f: InstalledFile) =
                    same f.TargetPath target
                    || f.TargetPath.StartsWith(target.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase)

                let isUsers (f: InstalledFile) =
                    not (isNull f.Component) && isUserPart f.Component && under f

                let taking = files |> Array.filter isUsers

                if taking.Length = 0 then
                    { Success = false; Message = "That file is not one you added here." }
                elif not (canWriteTo (Path.GetDirectoryName(exePath))) then
                    { Success = false; Message = elevationNeededMessage }
                else
                    let baseline = baselineOf game exePath

                    for entry in Array.rev taking do
                        try
                            if entry.WasExisting && File.Exists(entry.BackupPath) then
                                File.Copy(entry.BackupPath, entry.TargetPath, true)
                            elif not entry.WasExisting && File.Exists(entry.TargetPath) then
                                File.Delete(entry.TargetPath)
                        with _ ->
                            ()

                    dropEmptyTree target
                    let keeping = files |> Array.filter (fun f -> not (isUsers f))
                    writeRecord game exePath (choicesOfRecord m choices) baseline keeping
                    { Success = true; Message = Path.GetFileName(target) + " removed" }
        with ex ->
            { Success = false; Message = "Could not remove the file: " + ex.Message }

    /// What an advanced install is made of, for a community post: every part
    /// on the game in install order - without the user's own files. Their
    /// extras, add-ons and plugins are theirs, not the mod's, and say nothing
    /// to someone else about how DLSS 5 was put on this game.
    let sharedParts (game: GameItem) (exePath: string) : string[] =
        match readManifest game with
        | None -> [||]
        | Some m ->
            let exeDir = try Path.GetDirectoryName(exePath) with _ -> ""
            let mode = if isNull m.Mode then "" else m.Mode

            let present =
                filesOf m
                |> Array.filter (fun f -> not (String.IsNullOrWhiteSpace(f.TargetPath)))
                |> Array.map (partOf mode exeDir)
                |> Set.ofArray

            installOrder |> Array.filter (fun p -> present.Contains p && p <> partExtras)

    /// "Remove everything": the one-click removal, then the folders the user's
    /// own per-game extras brought - the removal empties them but, not being
    /// the mod's, does not know to take them away.
    let removeEverything (game: GameItem) (exePath: string) (plan: InstallPlan option) (report: Progress) : InstallOutcome =
        let folders =
            userFiles game exePath
            |> Array.filter (fun (part, path) -> part = partUserExtra && Directory.Exists(path))
            |> Array.map snd

        let outcome = uninstall game exePath plan report

        if outcome.Success then
            for f in folders do
                dropEmptyTree f

        outcome

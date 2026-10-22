namespace DLSS_5_MANAGER.ViewModels

open System
open System.Collections.Generic
open System.Collections.ObjectModel
open System.IO
open Avalonia.Threading
open DLSS_5_MANAGER.Models
open DLSS_5_MANAGER.Services

/// What the advanced sheet reads from the Manage sheet it stands in for: the
/// game, what the game turned out to be, and the choices kept in Settings.
type AdvancedContext =
    { Game: GameItem
      ExePath: string
      Plan: ModInstaller.InstallPlan option
      DetectedApi: string
      DetectedArch: string
      DetectedDlss: bool
      IsEmulator: bool
      IsAmdMode: bool
      /// The overlay's look and key; `Enabled` is this game's overlay choice.
      Overlay: ModInstaller.OverlayOptions
      OverlayReady: bool }

/// One pick inside a part's row: ReShade's API, RenoDX's build, OptiScaler's API.
type AdvancedOptionViewModel(key: string, label: unit -> string, pick: string -> unit) =
    inherit ViewModelBase()

    let mutable active = false

    member _.Key = key
    member _.Label = label ()

    member this.IsActive
        with get () = active
        and set (value: bool) = this.SetProperty(&active, value) |> ignore

    member _.Pick() = pick key
    member this.Relabel() = this.RaisePropertyChanged("Label")

/// One file the user added to this game by hand - an add-on under ReShade, a
/// plugin under OptiScaler - with its own way off.
type AdvancedUserFileViewModel(path: string) =
    inherit ViewModelBase()

    member _.Path = path
    member _.Name = Path.GetFileName(path)

/// One part of the mod in the advanced sheet: a switch for whether it should
/// be on the game, and a line saying what pressing Apply will do about it.
type AdvancedPartViewModel(id: string, toggled: unit -> unit, picked: string -> string -> unit) =
    inherit ViewModelBase()

    let userFiles = ObservableCollection<AdvancedUserFileViewModel>()
    let mutable canAddUserFile = false
    let mutable isShown = true
    let mutable wanted = false
    let mutable status = ""
    /// "on" installed, "add" / "remove" a change waiting, "warn" something to
    /// know, "idle" nothing to do.
    let mutable tone = "idle"
    let mutable canToggle = true
    let mutable showDownload = false
    let mutable where = ""
    let mutable filesTip = ""
    let options = ObservableCollection<AdvancedOptionViewModel>()

    member _.Id = id
    member _.Name = Localization.current.AdvPartName id
    member _.Description = Localization.current.AdvPartDesc id

    /// The switch in the row. Moving it is a change of plan, nothing more -
    /// the game is only touched by Apply.
    member this.IsWanted
        with get () = wanted
        and set (value: bool) =
            if wanted <> value then
                wanted <- value
                this.RaisePropertyChanged("IsWanted")
                toggled ()

    /// The sheet setting it itself - a preset, a reload - which is not the
    /// user changing the plan.
    member this.SetWanted(value: bool) =
        if wanted <> value then
            wanted <- value
            this.RaisePropertyChanged("IsWanted")

    member _.Options = options
    member _.HasOptions = options.Count > 0

    member this.AddOption(key: string, label: unit -> string) =
        options.Add(AdvancedOptionViewModel(key, label, (fun k -> picked id k)))
        this.RaisePropertyChanged("HasOptions")

    member _.MarkOption(key: string) =
        for o in options do
            o.IsActive <- (o.Key = key)

    member _.StatusText = status
    member _.IsToneOn = tone = "on"
    member _.IsToneAdd = tone = "add"
    member _.IsToneRemove = tone = "remove"
    member _.IsToneWarn = tone = "warn"
    member _.IsToneIdle = tone = "idle"

    member this.SetStatus(text: string, newTone: string) =
        if status <> text || tone <> newTone then
            status <- text
            tone <- newTone

            for name in [ "StatusText"; "IsToneOn"; "IsToneAdd"; "IsToneRemove"; "IsToneWarn"; "IsToneIdle" ] do
                this.RaisePropertyChanged(name)

    member this.CanToggle
        with get () = canToggle
        and set (value: bool) = this.SetProperty(&canToggle, value) |> ignore

    /// A cloud add-on not fetched yet: the row offers the download.
    member this.ShowDownload
        with get () = showDownload
        and set (value: bool) = this.SetProperty(&showDownload, value) |> ignore

    member this.Where
        with get () = where
        and set (value: string) =
            if this.SetProperty(&where, (if isNull value then "" else value)) then
                this.RaisePropertyChanged("HasWhere")

    member _.HasWhere = where <> ""

    /// The files the restore point credits to this part, for the tooltip.
    member this.FilesTip
        with get () = filesTip
        and set (value: string) =
            if this.SetProperty(&filesTip, (if isNull value then "" else value)) then
                this.RaisePropertyChanged("HasFiles")

    member _.HasFiles = filesTip <> ""

    member _.DownloadText = Localization.current.AssetDownload

    /// The row itself: AMD's is there only with AMD mode on in Settings (or
    /// while its files are still on the game, so they can come off).
    member this.IsShown
        with get () = isShown
        and set (value: bool) = this.SetProperty(&isShown, value) |> ignore

    // ---- the user's own files: "Add addon" under ReShade, "Add plugin"
    // under OptiScaler, a file or folder for this game under the extras ------
    member _.SupportsUserFiles =
        id = AdvancedInstall.partReShade || id = AdvancedInstall.partOptiScaler || id = AdvancedInstall.partExtras

    /// The per-game extras take a whole folder as well as a file.
    member _.SupportsUserFolder = id = AdvancedInstall.partExtras

    member _.UserFilePart =
        if id = AdvancedInstall.partOptiScaler then AdvancedInstall.partUserPlugin
        elif id = AdvancedInstall.partExtras then AdvancedInstall.partUserExtra
        else AdvancedInstall.partUserAddon

    member _.AddUserFileText =
        if id = AdvancedInstall.partOptiScaler then Localization.current.AdvAddPlugin
        elif id = AdvancedInstall.partExtras then Localization.current.AdvAddExtraFile
        else Localization.current.AdvAddAddon

    member _.AddUserFolderText = Localization.current.AdvAddExtraFolder

    member _.AddUserFileTip =
        if id = AdvancedInstall.partOptiScaler then Localization.current.AdvAddPluginTip
        elif id = AdvancedInstall.partExtras then Localization.current.AdvAddExtraTip
        else Localization.current.AdvAddAddonTip

    member _.RemoveUserFileTip = Localization.current.AdvRemoveFile

    /// Only where there is something to load it: a ReShade on the game for an
    /// add-on, an OptiScaler for a plugin.
    member this.CanAddUserFile
        with get () = canAddUserFile
        and set (value: bool) = this.SetProperty(&canAddUserFile, value) |> ignore

    member _.UserFiles = userFiles
    member _.HasUserFiles = userFiles.Count > 0

    member this.SetUserFiles(paths: string[]) =
        let now = userFiles |> Seq.map (fun f -> f.Path) |> Array.ofSeq

        if now <> paths then
            userFiles.Clear()

            for p in paths do
                userFiles.Add(AdvancedUserFileViewModel(p))

            this.RaisePropertyChanged("HasUserFiles")

    member this.Relabel() =
        this.RaisePropertyChanged("Name")
        this.RaisePropertyChanged("Description")
        this.RaisePropertyChanged("DownloadText")
        this.RaisePropertyChanged("AddUserFileText")
        this.RaisePropertyChanged("AddUserFolderText")
        this.RaisePropertyChanged("AddUserFileTip")
        this.RaisePropertyChanged("RemoveUserFileTip")

        for o in options do
            o.Relabel()

/// The advanced install sheet (Settings -> Features -> Advanced install).
///
/// The switches say what should be on the game; the status beside each says
/// what Apply will do to get it there. The route buttons at the top tick what
/// the one-click install would, and the user is free to change any of it.
/// The work itself is `AdvancedInstall` - this only keeps the plan.
type AdvancedInstallViewModel(context: unit -> AdvancedContext option, finished: string -> bool -> unit) as this =
    inherit ViewModelBase()

    let partsById = Dictionary<string, AdvancedPartViewModel>()

    let mutable states: Map<string, AdvancedInstall.PartState> = Map.empty
    let mutable basis = ModInstaller.Dx12Auto
    let mutable build = ModInstaller.Bit64
    let mutable reshadeApi = "auto"
    let mutable optiApi = ModInstaller.OptiDx12
    let mutable renoBuild = "multipass"
    /// What the last route button ticked; empty when none was pressed.
    let mutable presetParts: Set<string> = Set.empty
    /// The route and build of the restore point on the game ("" = none).
    let mutable installedRoute = ""
    /// The user has changed something in this sheet. Until then a fresh scan
    /// may still re-decide the plan from what the game turned out to be.
    let mutable touched = false
    let mutable generation = 0
    let mutable pendingInstall: Set<string> = Set.empty
    let mutable pendingRemove: Set<string> = Set.empty

    /// The install holds changes made here, so the one-click sheet stays
    /// closed to this game (`AdvancedInstall.isEasyLocked`).
    let mutable easyLocked = false

    /// The user's own files on the game, as (part, path).
    let mutable userFileList: (string * string)[] = [||]

    /// AMD mode in Settings, as it was when the sheet opened.
    let mutable amdMode = false

    /// AMD mode is on and nothing the user built is on the game: the sheet
    /// goes to the AMD route by itself, as the one-click sheet does.
    let mutable autoAmd = false

    /// What the switches and picks were when the sheet last settled - a route
    /// ticked, or the game's install read back. Anything else is the user's
    /// own set: the CUSTOM tab.
    let mutable baseWanted: Set<string> = Set.empty
    let mutable baseVariants = ("auto", "standard", ModInstaller.OptiDx12, ModInstaller.Bit64)

    /// The install on the game is itself one the user built in this sheet -
    /// not what any route would have put there.
    let mutable openedCustom = false

    let mutable busy = false
    let mutable progress = 0.0
    let mutable statusText = ""
    let mutable resultText = ""
    let mutable resultIsError = false
    let mutable resultExpanded = false

    let make (id: string) =
        let part =
            AdvancedPartViewModel(id, (fun () -> this.OnPartToggled()), (fun part key -> this.PickOption(part, key)))

        partsById.[id] <- part
        part

    let group (ids: string list) =
        ObservableCollection<AdvancedPartViewModel>(ids |> List.map make)

    // One heading each: ReShade, OptiScaler, RenoDX, deep-fried chicken, what
    // only DirectX 9 and 32-bit games use, AMD, the other DLSS 5 add-ons,
    // NVIDIA's files, and the dynamic extra files.
    let reshadeParts = group [ AdvancedInstall.partReShade; AdvancedInstall.partEffects ]
    let optiParts = group [ AdvancedInstall.partOptiScaler ]
    let dx9Parts = group [ AdvancedInstall.partDgVoodoo; AdvancedInstall.partHost64 ]
    let amdParts = group [ AdvancedInstall.partAmd ]
    let renoParts = group [ AdvancedInstall.partRenoDx; AdvancedInstall.partMfgUnlock ]
    let deepFriedParts = group [ AdvancedInstall.partDeepFried ]
    let dlss5AddonParts = group [ AdvancedInstall.partFeed; AdvancedInstall.partNeural; AdvancedInstall.partOverlay ]

    let nvidiaParts =
        group
            [ AdvancedInstall.partModel; AdvancedInstall.partModelSpread; AdvancedInstall.partDlss
              AdvancedInstall.partStreamline ]

    let extraParts = group [ AdvancedInstall.partExtras ]

    let part (id: string) = partsById.[id]

    let modeOfKey (key: string) =
        match key with
        | "optiscaler" -> ModInstaller.OptiScalerMode
        | "dx11" -> ModInstaller.Dx11
        | "dx9" -> ModInstaller.Dx9
        | "vulkan" -> ModInstaller.VulkanMode
        | "emulator" -> ModInstaller.Emulator
        | "amd" -> ModInstaller.AmdMode
        | _ -> ModInstaller.Dx12Auto

    do
        let loc () = Localization.current
        let reshade = part AdvancedInstall.partReShade
        reshade.AddOption("auto", (fun () -> loc().AdvOptAuto))
        reshade.AddOption("dxgi", (fun () -> "DXGI"))
        reshade.AddOption("d3d9", (fun () -> "D3D9"))
        reshade.AddOption("opengl", (fun () -> "OpenGL"))
        reshade.AddOption("vulkan", (fun () -> "Vulkan"))
        let reno = part AdvancedInstall.partRenoDx
        reno.AddOption("standard", (fun () -> loc().AdvOptStandard))
        reno.AddOption("multipass", (fun () -> "Multipass"))
        let opti = part AdvancedInstall.partOptiScaler
        opti.AddOption("dx12", (fun () -> "DirectX 12"))
        opti.AddOption("vulkan", (fun () -> "Vulkan"))

    // ---------------------------------------------------------------------
    // THE PLAN
    // ---------------------------------------------------------------------
    member _.ReShadeParts = reshadeParts
    member _.OptiScalerParts = optiParts
    member _.Dx9Parts = dx9Parts
    member _.AmdParts = amdParts
    member _.RenoDxParts = renoParts
    member _.DeepFriedParts = deepFriedParts
    member _.Dlss5AddonParts = dlss5AddonParts
    member _.NvidiaParts = nvidiaParts
    member _.ExtraParts = extraParts

    /// The AMD heading goes with its row.
    member _.ShowAmdGroup = (part AdvancedInstall.partAmd).IsShown

    /// What would actually go on or stay on the game: a part with nothing to
    /// do here (no Streamline to update, no extras) says nothing either way.
    member private _.Effective(set: Set<string>) =
        set
        |> Set.filter (fun id ->
            match states |> Map.tryFind id with
            | Some s -> s.Installed || (s.Idle = "" && s.Blocked = "")
            | None -> true)

    member private this.SettleBase() =
        baseWanted <- this.WantedSet()
        baseVariants <- (reshadeApi, renoBuild, optiApi, build)

    member private _.WantedSet() =
        partsById.Values |> Seq.filter (fun p -> p.IsWanted) |> Seq.map (fun p -> p.Id) |> Set.ofSeq

    member private _.Choices(ctx: AdvancedContext) : AdvancedInstall.Choices =
        { Build = build
          ReShadeApi = reshadeApi
          OptiApi = optiApi
          RenoDxBuild = renoBuild
          Overlay = ctx.Overlay
          Basis = basis
          IsEmulator = ctx.IsEmulator }

    /// The route the one-click sheet would open this game on - the same
    /// reading `MainViewModel.DetectTarget` makes.
    member private _.DetectedBasis(ctx: AdvancedContext) =
        if ctx.IsEmulator then ModInstaller.Emulator
        elif ctx.IsAmdMode then ModInstaller.AmdMode
        elif ctx.DetectedApi = "dx9" then ModInstaller.Dx9
        elif ctx.DetectedApi = "dx10" then ModInstaller.Dx11
        elif ctx.DetectedDlss then ModInstaller.OptiScalerMode
        elif ctx.DetectedApi = "dx11" then ModInstaller.Dx11
        else ModInstaller.Dx12Auto

    /// Ticks what the route would install - `AdvancedInstall.recommend`.
    member private this.TickPreset(ctx: AdvancedContext) =
        let preset =
            AdvancedInstall.recommend basis build ctx.ExePath ctx.DetectedApi (ctx.Overlay.Enabled && ctx.OverlayReady)

        reshadeApi <- preset.ReShadeApi
        optiApi <- preset.OptiApi
        renoBuild <- preset.RenoDxBuild
        presetParts <- preset.Parts

        for p in partsById.Values do
            p.SetWanted(preset.Parts.Contains p.Id)

        openedCustom <- false
        this.SettleBase()

    /// Where the sheet starts: what is on the game when something is, the
    /// route the game suits when nothing is.
    member private this.Init(ctx: AdvancedContext) =
        let (route, arch) = ModInstaller.installedRouteAndArch ctx.Game
        installedRoute <- route
        presetParts <- Set.empty

        // AMD mode in Settings: the AMD route, by itself - on a fresh game and
        // over a one-click install alike, as the one-click sheet does. Not over
        // an install the user built here: that one is already theirs.
        autoAmd <-
            ctx.IsAmdMode && not ctx.IsEmulator && route <> "amd" && not (AdvancedInstall.isEasyLocked ctx.Game)

        if autoAmd then
            basis <- ModInstaller.AmdMode
            build <- ModInstaller.Bit64
            optiApi <- ModInstaller.OptiDx12
        elif route <> "" then
            basis <- modeOfKey route
            build <- (if arch = "32" then ModInstaller.Bit32 else ModInstaller.Bit64)
            optiApi <- (if ModInstaller.installedOptiApi ctx.Game = "vulkan" then ModInstaller.OptiVulkan else ModInstaller.OptiDx12)
        else
            basis <- this.DetectedBasis(ctx)
            build <- (if ctx.DetectedArch = "32" then ModInstaller.Bit32 else ModInstaller.Bit64)
            optiApi <- ModInstaller.OptiDx12

        reshadeApi <- "auto"
        renoBuild <- "multipass"
        this.RaiseChoices()

    /// Once the game has been read: the switches follow what is on it, or -
    /// with nothing of ours there yet - the route the game suits.
    member private this.WantFromGame(ctx: AdvancedContext) =
        let anyInstalled = states |> Map.exists (fun _ s -> s.Installed)

        if anyInstalled && not autoAmd then
            for p in partsById.Values do
                match states |> Map.tryFind p.Id with
                | Some s -> p.SetWanted(s.Installed || s.Present)
                | None -> p.SetWanted(false)

            let variant (id: string) =
                match states |> Map.tryFind id with
                | Some s when s.Installed -> s.Variant
                | _ -> ""

            // The build on the game when RenoDX is there; the default build
            // (Multipass) for a RenoDX ticked from here.
            match variant AdvancedInstall.partRenoDx with
            | "standard" -> renoBuild <- "standard"
            | _ -> renoBuild <- "multipass"

            match variant AdvancedInstall.partOptiScaler with
            | "vulkan" -> optiApi <- ModInstaller.OptiVulkan
            | "dx12" -> optiApi <- ModInstaller.OptiDx12
            | _ -> ()

            // "Auto" stays when it lands where ReShade already is; otherwise
            // the slot it is on is picked, so nothing reads as a change.
            match variant AdvancedInstall.partReShade with
            | ("dxgi" | "d3d9" | "opengl" | "vulkan") as installed ->
                let auto =
                    AdvancedInstall.reshadeVariantFor { this.Choices(ctx) with ReShadeApi = "auto" } ctx.ExePath (this.WantedSet())

                reshadeApi <- (if auto = installed then "auto" else installed)
            | _ -> reshadeApi <- "auto"

            this.SettleBase()

            // An install built in this sheet that no route would have made is
            // shown as CUSTOM from the start.
            let route =
                AdvancedInstall.recommend basis build ctx.ExePath ctx.DetectedApi (ctx.Overlay.Enabled && ctx.OverlayReady)

            let wanted = this.WantedSet()

            openedCustom <-
                easyLocked
                && (this.Effective(wanted) <> this.Effective(route.Parts)
                    || build <> route.Build
                    || (wanted.Contains AdvancedInstall.partRenoDx && renoBuild <> route.RenoDxBuild)
                    || (wanted.Contains AdvancedInstall.partReShade && reshadeApi <> route.ReShadeApi))
        else
            this.TickPreset(ctx)

        this.RaiseChoices()

    // ---------------------------------------------------------------------
    // WHAT APPLY WILL DO
    // ---------------------------------------------------------------------
    member private this.Recompute() =
        match context () with
        | None -> ()
        | Some ctx ->
            let loc = Localization.current
            let c = this.Choices(ctx)
            let wantedSet = this.WantedSet()
            let is32 = build = ModInstaller.Bit32
            let exeDir = try Path.GetDirectoryName(ctx.ExePath) with _ -> ""
            let mutable toInstall = Set.empty
            let mutable toRemove = Set.empty

            (part AdvancedInstall.partReShade).MarkOption(reshadeApi)
            (part AdvancedInstall.partRenoDx).MarkOption(renoBuild)
            (part AdvancedInstall.partOptiScaler).MarkOption(if optiApi = ModInstaller.OptiVulkan then "vulkan" else "dx12")

            for p in partsById.Values do
                let id = p.Id

                let chosen =
                    match id with
                    | x when x = AdvancedInstall.partReShade -> AdvancedInstall.reshadeVariantFor c ctx.ExePath wantedSet
                    | x when x = AdvancedInstall.partRenoDx -> renoBuild
                    | x when x = AdvancedInstall.partOptiScaler ->
                        if optiApi = ModInstaller.OptiVulkan then "vulkan" else "dx12"
                    | x when
                        x = AdvancedInstall.partFeed
                        || x = AdvancedInstall.partOverlay
                        || x = AdvancedInstall.partDeepFried
                        || x = AdvancedInstall.partNeural
                        || x = AdvancedInstall.partModel
                        ->
                        ModInstaller.archKey build
                    | _ -> ""

                p.Where <-
                    match id with
                    | x when x = AdvancedInstall.partDeepFried || x = AdvancedInstall.partNeural || x = AdvancedInstall.partModel ->
                        if is32 then loc.AdvWhereHost64 else loc.AdvWhereGame
                    | x when x = AdvancedInstall.partFeed || x = AdvancedInstall.partOverlay ->
                        loc.AdvWhereGame + (if is32 then " · 32-bit" else " · 64-bit")
                    | x when x = AdvancedInstall.partHost64 -> loc.AdvWhereHost64
                    | x when
                        x = AdvancedInstall.partReShade
                        || x = AdvancedInstall.partEffects
                        || x = AdvancedInstall.partDgVoodoo
                        || x = AdvancedInstall.partRenoDx
                        || x = AdvancedInstall.partMfgUnlock
                        || x = AdvancedInstall.partOptiScaler
                        || x = AdvancedInstall.partAmd
                        ->
                        loc.AdvWhereGame
                    | _ -> ""

                match states |> Map.tryFind id with
                | None ->
                    p.SetStatus(loc.AdvStatusChecking, "idle")
                    p.CanToggle <- true
                    p.ShowDownload <- false
                    p.FilesTip <- ""
                | Some s ->
                    let differs =
                        s.Installed && chosen <> "" && s.Variant <> "" && s.Variant <> "custom" && chosen <> s.Variant

                    p.CanToggle <- s.Installed || s.Blocked <> "missing"
                    p.ShowDownload <- s.Blocked = "download" && not s.Installed

                    p.FilesTip <-
                        if s.Files.Length = 0 then
                            ""
                        else
                            let rel (f: string) =
                                try
                                    let r = Path.GetRelativePath(exeDir, f)
                                    if r.StartsWith("..") then f else r
                                with _ ->
                                    f

                            let shown = s.Files |> Array.truncate 14 |> Array.map rel
                            let more = s.Files.Length - shown.Length

                            loc.AdvFilesOnGame
                            + "\n"
                            + String.Join("\n", shown)
                            + (if more > 0 then sprintf "\n+%d" more else "")

                    if p.IsWanted then
                        if s.Installed then
                            if differs then
                                toInstall <- toInstall.Add id
                                toRemove <- toRemove.Add id
                                p.SetStatus(loc.AdvStatusWillReinstall, "add")
                            elif s.Incomplete then
                                toInstall <- toInstall.Add id
                                p.SetStatus(loc.AdvStatusWillRepair, "warn")
                            else
                                p.SetStatus(loc.AdvStatusInstalled, "on")
                        elif s.Present then
                            p.SetStatus(loc.AdvStatusPresent, "on")
                        elif s.Blocked = "missing" then
                            p.SetStatus(loc.AdvStatusMissing, "warn")
                        elif s.Blocked = "download" then
                            p.SetStatus(loc.AdvStatusDownload, "warn")
                        elif s.Idle <> "" then
                            p.SetStatus(
                                (match s.Idle with
                                 | "uptodate" -> loc.AdvStatusUpToDate
                                 | "nostreamline" -> loc.AdvStatusNoStreamline
                                 | "nofolders" -> loc.AdvStatusNoFolders
                                 | _ -> loc.AdvStatusNoExtras),
                                "idle"
                            )
                        else
                            toInstall <- toInstall.Add id
                            p.SetStatus(loc.AdvStatusWillInstall, "add")
                    elif s.Installed then
                        toRemove <- toRemove.Add id
                        p.SetStatus(loc.AdvStatusWillRemove, "remove")
                    elif s.Present then
                        p.SetStatus(loc.AdvStatusPresent, "idle")
                    elif s.Blocked = "missing" then
                        p.SetStatus(loc.AdvStatusMissing, "idle")
                    else
                        p.SetStatus(loc.AdvStatusNotInstalled, "idle")

            pendingInstall <- toInstall
            pendingRemove <- toRemove

            let state (id: string) = states |> Map.tryFind id

            // AMD's files only with AMD mode on - or while they are still on
            // the game, so they can be taken off.
            (part AdvancedInstall.partAmd).IsShown <-
                amdMode || (match state AdvancedInstall.partAmd with Some s -> s.Installed | None -> false)

            // "Add addon" wants a ReShade to load it, "Add plugin" an OptiScaler.
            let reshade = part AdvancedInstall.partReShade
            let opti = part AdvancedInstall.partOptiScaler

            reshade.CanAddUserFile <-
                (match state AdvancedInstall.partReShade with
                 | Some s -> s.Installed || s.Present
                 | None -> false)

            opti.CanAddUserFile <-
                (match state AdvancedInstall.partOptiScaler with
                 | Some s -> s.Installed
                 | None -> false)

            // A file for this game alone goes with its install - so there has
            // to be one.
            let extras = part AdvancedInstall.partExtras
            extras.CanAddUserFile <- installedRoute <> ""

            let filesOf (kind: string) =
                userFileList |> Array.filter (fun (k, _) -> k = kind) |> Array.map snd

            reshade.SetUserFiles(filesOf AdvancedInstall.partUserAddon)
            opti.SetUserFiles(filesOf AdvancedInstall.partUserPlugin)
            extras.SetUserFiles(filesOf AdvancedInstall.partUserExtra)

            for name in
                [ "PendingCount"; "HasPending"; "CanApply"; "ApplyText"; "HasConflict"; "IsCustomized"
                  "CanRemoveAll"; "ShowOptiKey"; "ShowReShadeKey"; "ShowOverlayKey"; "ShowKeysCard"
                  "CanGoEasy"; "EasyModeTip"; "IsEasyLocked"; "ShowAmdGroup" ] do
                this.RaisePropertyChanged(name)

            // The CUSTOM tab and the route tabs follow every change.
            this.RaiseChoices()

    /// Reads the game again, off the UI thread. `resetWanted` lets the
    /// switches follow what was found.
    member private this.ReloadStates(resetWanted: bool) =
        match context () with
        | None -> ()
        | Some ctx ->
            generation <- generation + 1
            let gen = generation
            let c = this.Choices(ctx)

            System.Threading.Tasks.Task.Run(fun () ->
                let result =
                    try
                        Some(
                            AdvancedInstall.readState ctx.Game ctx.ExePath ctx.Plan c,
                            fst (ModInstaller.installedRouteAndArch ctx.Game),
                            AdvancedInstall.isEasyLocked ctx.Game,
                            AdvancedInstall.userFiles ctx.Game ctx.ExePath
                        )
                    with _ ->
                        None

                Dispatcher.UIThread.Post(fun () ->
                    if gen = generation then
                        match result with
                        | Some(st, route, locked, files) ->
                            states <- st
                            installedRoute <- route
                            easyLocked <- locked
                            userFileList <- files
                            if resetWanted then this.WantFromGame(ctx)
                            this.RaiseChoices()
                            this.Recompute()
                        | None -> ()))
            |> ignore

    // ---------------------------------------------------------------------
    // OPENING AND FOLLOWING THE MANAGE SHEET
    // ---------------------------------------------------------------------
    /// A game was opened in advanced mode.
    member this.Open() =
        match context () with
        | None -> ()
        | Some ctx ->
            touched <- false
            states <- Map.empty
            userFileList <- [||]
            easyLocked <- AdvancedInstall.isEasyLocked ctx.Game
            amdMode <- ctx.IsAmdMode
            this.RaisePropertyChanged("ShowAmdPreset")
            this.RaisePropertyChanged("PresetColumns")
            resultText <- ""
            resultIsError <- false
            resultExpanded <- false
            progress <- 0.0
            statusText <- ""
            this.RaiseResult()
            this.Init(ctx)

            for p in partsById.Values do
                p.SetWanted(false)

            this.Recompute()
            this.ReloadStates(true)

    /// The Manage sheet read the game again (a rescan, another executable,
    /// the end of an install). Untouched, the plan is made again from what was
    /// found; otherwise only the statuses move.
    member this.Refresh() =
        if not busy then
            match context () with
            | Some ctx when not touched ->
                this.Init(ctx)
                this.ReloadStates(true)
            | Some _ -> this.ReloadStates(false)
            | None -> ()

    member this.Relabel() =
        for p in partsById.Values do
            p.Relabel()

        this.Recompute()
        this.RaisePropertyChanged("ApplyText")
        this.RaisePropertyChanged("ResultToggleText")

    // ---------------------------------------------------------------------
    // THE USER'S CHANGES
    // ---------------------------------------------------------------------
    member this.OnPartToggled() =
        touched <- true
        this.Recompute()

    member this.PickOption(partId: string, key: string) =
        touched <- true

        if partId = AdvancedInstall.partReShade then reshadeApi <- key
        elif partId = AdvancedInstall.partRenoDx then renoBuild <- key
        elif partId = AdvancedInstall.partOptiScaler then
            optiApi <- (if key = "vulkan" then ModInstaller.OptiVulkan else ModInstaller.OptiDx12)

        this.Recompute()
        this.ReloadStates(false)

    /// A route button: its own build, exactly as the one-click sheet's route
    /// buttons do it, and what it installs ticked.
    member this.ApplyPreset(key: string) =
        match context () with
        | Some ctx when not busy ->
            touched <- true
            basis <- modeOfKey key
            build <- (if basis = ModInstaller.Dx9 then ModInstaller.Bit32 else ModInstaller.Bit64)
            this.TickPreset(ctx)
            this.RaiseChoices()
            this.Recompute()
            this.ReloadStates(false)
        | _ -> ()

    /// The build picker. While the switches are still the route's own
    /// choice, they follow the build; once changed by hand they stay.
    member this.SetBuild(key: string) =
        match context () with
        | Some ctx when not busy ->
            let wanted = if key = "32" then ModInstaller.Bit32 else ModInstaller.Bit64

            if build <> wanted then
                touched <- true
                let followPreset = not presetParts.IsEmpty && this.WantedSet() = presetParts
                build <- wanted

                if followPreset then
                    let preset =
                        AdvancedInstall.recommend basis build ctx.ExePath ctx.DetectedApi (ctx.Overlay.Enabled && ctx.OverlayReady)

                    presetParts <- preset.Parts

                    for p in partsById.Values do
                        p.SetWanted(preset.Parts.Contains p.Id)

                    this.SettleBase()

                this.RaiseChoices()
                this.Recompute()
                this.ReloadStates(false)
        | _ -> ()

    // ---------------------------------------------------------------------
    // WHAT THE SHEET SHOWS
    // ---------------------------------------------------------------------
    // A route's tab is lit while the switches are still that route's own;
    // once the user changed them, the CUSTOM tab is.
    member this.IsBasisOpti = basis = ModInstaller.OptiScalerMode && not this.IsCustomized
    member this.IsBasisDx12 = basis = ModInstaller.Dx12Auto && not this.IsCustomized
    member this.IsBasisDx11 = basis = ModInstaller.Dx11 && not this.IsCustomized
    member this.IsBasisDx9 = basis = ModInstaller.Dx9 && not this.IsCustomized
    member this.IsBasisVulkan = basis = ModInstaller.VulkanMode && not this.IsCustomized
    member this.IsBasisEmulator = basis = ModInstaller.Emulator && not this.IsCustomized
    member this.IsBasisAmd = basis = ModInstaller.AmdMode && not this.IsCustomized

    member _.IsOptiRouteInstalled = installedRoute = "optiscaler"
    member _.IsDx12RouteInstalled = installedRoute = "dx12"
    member _.IsDx11RouteInstalled = installedRoute = "dx11"
    member _.IsDx9RouteInstalled = installedRoute = "dx9"
    member _.IsVulkanRouteInstalled = installedRoute = "vulkan"
    member _.IsEmulatorRouteInstalled = installedRoute = "emulator"
    member _.IsAmdRouteInstalled = installedRoute = "amd"

    member _.IsBuild64 = build = ModInstaller.Bit64
    member _.IsBuild32 = build = ModInstaller.Bit32

    /// The AMD preset is there only with AMD mode on in Settings - that is the
    /// setting that says this machine has the card it is for.
    member _.ShowAmdPreset = amdMode

    /// CUSTOM first, then the routes (AMD only with AMD mode on).
    member _.PresetColumns = if amdMode then 8 else 7

    // ---- back to the one-click sheet ------------------------------------
    /// Changes made here are on the game.
    member _.IsEasyLocked = easyLocked

    /// The one-click sheet can take this game: nothing of the advanced sheet's
    /// is on it, and - on a game with an install - nothing is waiting to be
    /// applied. A fresh game's ticks are only a suggestion, so they never hold
    /// the user here.
    member this.CanGoEasy =
        not busy && not easyLocked && (installedRoute = "" || not touched || not this.HasPending)

    member this.EasyModeTip =
        if easyLocked then Localization.current.ModeEasyLocked
        elif installedRoute <> "" && touched && this.HasPending then Localization.current.ModeEasyPending
        else Localization.current.ModeEasyTip

    /// The CUSTOM tab: the user built this set - changed what a route ticked,
    /// or opened on an install built here that no route would have made.
    member this.IsCustomized =
        not states.IsEmpty
        && (openedCustom
            || this.WantedSet() <> baseWanted
            || (reshadeApi, renoBuild, optiApi, build) <> baseVariants)

    member private this.RaiseChoices() =
        for name in
            [ "IsBasisOpti"; "IsBasisDx12"; "IsBasisDx11"; "IsBasisDx9"; "IsBasisVulkan"; "IsBasisEmulator"
              "IsBasisAmd"; "IsOptiRouteInstalled"; "IsDx12RouteInstalled"; "IsDx11RouteInstalled"
              "IsDx9RouteInstalled"; "IsVulkanRouteInstalled"; "IsEmulatorRouteInstalled"; "IsAmdRouteInstalled"
              "IsBuild64"; "IsBuild32"; "IsCustomized" ] do
            this.RaisePropertyChanged(name)

    member _.PendingCount = (Set.union pendingInstall pendingRemove).Count
    member this.HasPending = this.PendingCount > 0
    member this.CanApply = not busy && this.HasPending

    member this.ApplyText =
        if this.PendingCount > 0 then Localization.current.AdvApplyCount(this.PendingCount)
        else Localization.current.AdvNoChanges

    /// Deep-fried chicken and RenoDX both switched on: the banner at the
    /// bottom. Said, never prevented.
    member _.HasConflict =
        (part AdvancedInstall.partRenoDx).IsWanted && (part AdvancedInstall.partDeepFried).IsWanted

    member _.CanRemoveAll =
        not busy
        && (installedRoute <> "" || states |> Map.exists (fun _ s -> s.Installed))

    // The CHANGE KEY rows, for the parts that have a menu to open.
    member _.ShowOptiKey =
        let s = states |> Map.tryFind AdvancedInstall.partOptiScaler
        (part AdvancedInstall.partOptiScaler).IsWanted || (s.IsSome && s.Value.Installed)

    member _.ShowReShadeKey =
        let s = states |> Map.tryFind AdvancedInstall.partReShade
        (part AdvancedInstall.partReShade).IsWanted || (s.IsSome && (s.Value.Installed || s.Value.Present))

    member _.ShowOverlayKey =
        let s = states |> Map.tryFind AdvancedInstall.partOverlay
        (part AdvancedInstall.partOverlay).IsWanted || (s.IsSome && s.Value.Installed)

    member this.ShowKeysCard = this.ShowOptiKey || this.ShowReShadeKey || this.ShowOverlayKey

    // ---------------------------------------------------------------------
    // RUNNING IT
    // ---------------------------------------------------------------------
    member _.IsBusy = busy
    member _.IsReady = not busy
    member _.Progress = progress
    member _.StatusText = statusText
    member _.HasResult = not (String.IsNullOrWhiteSpace(resultText))
    member _.ResultText = resultText

    member _.ResultSummary =
        let cut = resultText.IndexOf(" • ")
        if cut > 0 then resultText.Substring(0, cut) else resultText

    member _.HasResultDetails = resultText.Contains(" • ")
    member _.IsResultExpanded = resultExpanded
    member _.ResultToggleText = if resultExpanded then "HIDE" else "DETAILS"
    member _.IsResultError = resultIsError

    member this.ToggleResult() =
        resultExpanded <- not resultExpanded
        this.RaisePropertyChanged("IsResultExpanded")
        this.RaisePropertyChanged("ResultToggleText")

    member private this.RaiseResult() =
        for name in
            [ "HasResult"; "ResultText"; "ResultSummary"; "HasResultDetails"; "IsResultExpanded"; "ResultToggleText"
              "IsResultError"; "Progress"; "StatusText" ] do
            this.RaisePropertyChanged(name)

    member private this.SetBusy(value: bool) =
        busy <- value

        for name in [ "IsBusy"; "IsReady"; "CanApply"; "CanRemoveAll"; "CanGoEasy" ] do
            this.RaisePropertyChanged(name)

    member private this.Run(label: string, work: ModInstaller.Progress -> ModInstaller.InstallOutcome, kind: string) =
        this.SetBusy(true)
        resultText <- ""
        resultIsError <- false
        resultExpanded <- false
        progress <- 0.0
        statusText <- label
        this.RaiseResult()

        let report: ModInstaller.Progress =
            fun text p ->
                Dispatcher.UIThread.Post(fun () ->
                    statusText <- text
                    progress <- p * 100.0
                    this.RaisePropertyChanged("StatusText")
                    this.RaisePropertyChanged("Progress"))

        System.Threading.Tasks.Task.Run(fun () ->
            let outcome =
                try
                    work report
                with ex ->
                    { ModInstaller.InstallOutcome.Success = false
                      ModInstaller.InstallOutcome.Message = ex.Message }

            Dispatcher.UIThread.Post(fun () ->
                this.SetBusy(false)
                resultIsError <- not outcome.Success
                resultText <- outcome.Message
                progress <- (if outcome.Success then 100.0 else 0.0)
                statusText <- ""
                this.RaiseResult()
                finished kind outcome.Success
                this.ReloadStates(false)))
        |> ignore

    /// Everything switched on that is not on the game goes in; everything
    /// switched off that is comes off.
    member this.Apply() =
        match context () with
        | Some ctx when not busy && this.HasPending ->
            let c = this.Choices(ctx)
            let wanted = this.WantedSet()
            let install = pendingInstall
            let remove = pendingRemove
            touched <- true
            this.Run("Starting...", (fun report -> AdvancedInstall.apply ctx.Game ctx.ExePath ctx.Plan c wanted install remove report), "install")
        | _ -> ()

    /// "Add addon" / "Add plugin": one file of the user's own onto this game,
    /// straight away - it is a file the user picked, not a plan to apply.
    member this.AddUserFile(partId: string, source: string) =
        match context () with
        | Some ctx when not busy && not (String.IsNullOrWhiteSpace(source)) ->
            let kind =
                if partId = AdvancedInstall.partOptiScaler || partId = AdvancedInstall.partUserPlugin then
                    AdvancedInstall.partUserPlugin
                elif partId = AdvancedInstall.partExtras || partId = AdvancedInstall.partUserExtra then
                    AdvancedInstall.partUserExtra
                else
                    AdvancedInstall.partUserAddon

            let c = this.Choices(ctx)
            touched <- true
            this.Run("Adding...", (fun _ -> AdvancedInstall.addUserFile ctx.Game ctx.ExePath c kind source), "addfile")
        | _ -> ()

    /// "Add folder" under the dynamic extra files: a folder of the user's own,
    /// whole, beside this game only.
    member this.AddUserFolder(source: string) =
        match context () with
        | Some ctx when not busy && not (String.IsNullOrWhiteSpace(source)) ->
            let c = this.Choices(ctx)
            touched <- true
            this.Run("Adding...", (fun _ -> AdvancedInstall.addUserFolder ctx.Game ctx.ExePath c source), "addfile")
        | _ -> ()

    /// The × on one of those files: it comes off, and whatever of the game's
    /// it had replaced goes back.
    member this.RemoveUserFile(path: string) =
        match context () with
        | Some ctx when not busy && not (String.IsNullOrWhiteSpace(path)) ->
            let c = this.Choices(ctx)
            touched <- true
            this.Run("Removing...", (fun _ -> AdvancedInstall.removeUserFile ctx.Game ctx.ExePath c path), "removefile")
        | _ -> ()

    /// The one-click sheet's own removal: every part off, the game as it was.
    member this.RemoveAll() =
        match context () with
        | Some ctx when not busy ->
            touched <- true

            for p in partsById.Values do
                p.SetWanted(false)

            // Nothing on the game is nobody's set - not a custom one.
            openedCustom <- false
            this.SettleBase()
            presetParts <- Set.empty
            installedRoute <- ""
            this.RaiseChoices()
            this.Run("Removing...", (fun report -> AdvancedInstall.removeEverything ctx.Game ctx.ExePath ctx.Plan report), "remove")
        | _ -> ()

namespace DLSS_5_MANAGER.Services

open System
open System.IO
open System.Text.Json
open System.Collections.Generic

/// User interface translations.
///
/// Every string lives in `languages\languages.JSON` - one object per language
/// code, the same 44 keys in each. English is the fallback, so a key that is
/// ever missing shows readable text instead of blowing up.
///
/// The UI does not bind to the dictionary directly. It binds through a
/// `Strings` snapshot exposed as one property, so switching language means
/// swapping a single object and every label on screen re-reads itself - no
/// per-string change notifications to keep in sync.
module Localization =

    /// Display names stay in their own language: someone who cannot read the
    /// current UI language still has to be able to find their own.
    ///
    /// English first because it is what an unset app starts in and what every
    /// missing string falls back to; the rest in one order that does not
    /// change - alphabetical by the language's English name - so a list of
    /// seventeen can be scanned instead of read.
    let availableLanguages =
        [| "en", "English"
           "ar", "العربية"        // Arabic
           "zh", "中文"            // Chinese
           "fr", "Français"       // French
           "de", "Deutsch"        // German
           "hi", "हिन्दी"           // Hindi
           "id", "Bahasa Indonesia"
           "it", "Italiano"
           "ja", "日本語"          // Japanese
           "ko", "한국어"          // Korean
           "fa", "فارسی"          // Persian
           "pt", "Português"      // Portuguese
           "ru", "Русский"        // Russian
           "es", "Español"        // Spanish
           "tr", "Türkçe"         // Turkish
           "uk", "Українська"     // Ukrainian
           "vi", "Tiếng Việt" |]

    let private empty = Dictionary<string, string>() :> IReadOnlyDictionary<string, string>

    let private table =
        lazy
            (try
                let candidates =
                    [ Path.Combine(AppContext.BaseDirectory, "languages", "languages.JSON")
                      Path.Combine(AppContext.BaseDirectory, "languages", "languages.json") ]

                match candidates |> List.tryFind File.Exists with
                | None -> Dictionary<string, Dictionary<string, string>>()
                | Some path ->
                    let options = JsonSerializerOptions()
                    options.PropertyNameCaseInsensitive <- true

                    let parsed =
                        JsonSerializer.Deserialize<Dictionary<string, Dictionary<string, string>>>(
                            File.ReadAllText(path),
                            options
                        )

                    if isNull (box parsed) then Dictionary<string, Dictionary<string, string>>() else parsed
             with _ ->
                 Dictionary<string, Dictionary<string, string>>())

    let private forCode (code: string) : IReadOnlyDictionary<string, string> =
        match table.Value.TryGetValue(if isNull code then "" else code) with
        | true, v -> v :> IReadOnlyDictionary<string, string>
        | _ -> empty

    let isKnownLanguage (code: string) =
        availableLanguages |> Array.exists (fun (c, _) -> c = code)

    let displayName (code: string) =
        availableLanguages
        |> Array.tryFind (fun (c, _) -> c = code)
        |> Option.map snd
        |> Option.defaultValue "English"

    /// The language the OS is set to, when we have it - a first run should
    /// already be in the user's own language.
    let systemLanguage () =
        try
            let code = Globalization.CultureInfo.CurrentUICulture.TwoLetterISOLanguageName
            if isKnownLanguage code then code else "en"
        with _ ->
            "en"

    /// One language's strings, resolved once. Bound to as `Loc.Games` and
    /// friends; replacing the whole object is what refreshes the window.
    type Strings(code: string) =
        let primary = forCode code
        let fallback = forCode "en"

        let get (key: string) =
            match primary.TryGetValue(key) with
            | true, v when not (String.IsNullOrWhiteSpace(v)) -> v
            | _ ->
                match fallback.TryGetValue(key) with
                | true, v -> v
                | _ -> key

        member _.Code = code

        // ---- Header / navigation ----------------------------------------
        member _.ClickManage = get "click_manage"
        member _.DragMove = get "drag_move"
        member _.Games = get "games"
        member _.AddGame = get "add_game"
        member _.SearchGames = get "search_games"
        member _.Settings = get "settings"
        member _.Minimize = get "minimize"
        member _.Maximize = get "maximize"
        member _.Close = get "close"
        member _.BackToGames = get "back_to_games"

        // ---- Settings ----------------------------------------------------
        member _.SettingsPreferences = get "settings_preferences"
        member _.InterfaceNavigation = get "interface_navigation"
        member _.NavbarPosition = get "navbar_position"
        member _.TopBar = get "top_bar"
        member _.SideBar = get "side_bar"
        member _.VisualAtmosphere = get "visual_atmosphere"
        member _.ColorAtmosphere = get "color_atmosphere"
        member _.GeometricMotif = get "geometric_motif"
        member _.LibraryCache = get "library_cache"
        member _.FastCacheGameIndex = get "fast_cache_game_index"
        member _.RescanLibrary = get "rescan_library"
        member _.ClearCache = get "clear_cache"
        member _.StorageTitle = get "storage_title"
        member _.StorageUsed = get "storage_used"
        member _.StorageFreeable = get "storage_freeable"
        member _.StorageFreeUp = get "storage_free_up"
        member _.StorageHint = get "storage_hint"
        member _.StorageFreed = get "storage_freed"
        member _.ModPayloadFiles = get "mod_payload_files"
        member _.ModPayloadDesc = get "mod_payload_desc"
        member _.Replace = get "replace"
        member _.Restore = get "restore"

        // ---- Manage sheet ------------------------------------------------
        member _.Installed = get "installed"
        member _.NotInstalled = get "not_installed"
        member _.OpenGameFolder = get "open_game_folder"
        member _.ChangeExe = get "change_exe"
        member _.RemoveDlss = get "remove_dlss"
        member _.InstallMode = get "install_mode"
        member _.OptiScalerDesc = get "optiscaler_desc"

        // ---- Emulators ----------------------------------------------------
        member _.TabEmulators = get "tab_emulators"
        member _.NoEmulatorsTitle = get "no_emulators_title"
        member _.NoEmulatorsDesc = get "no_emulators_desc"
        member _.BtnAddEmulator = get "btn_add_emulator"
        member _.BtnDetectEmulators = get "btn_detect_emulators"

        // ---- Support & about ----------------------------------------------
        member _.SectionSupportTutorials = get "section_support_tutorials"
        member _.SupportTitle = get "support_title"
        member _.SupportDesc = get "support_desc"
        member _.BtnWatchTutorials = get "btn_watch_tutorials"
        member _.BtnSupportKofi = get "btn_support_kofi"
        member _.AppTitle = get "app_title"
        member _.CreditsBuiltBy = get "credits_built_by"
        member _.BtnCheckUpdates = get "btn_check_updates"
        member _.Copyright = get "copyright"

        // ---- Manage sheet, AMD mode, extras --------------------------------
        member _.GameExecutable = get "game_executable"
        member _.BtnInstall = get "btn_install"
        member _.AmdMode = get "amd_mode"
        member _.Beta = get "beta"
        member _.AmdTitle = get "amd_title"
        member _.AmdDesc = get "amd_desc"

        // ---- Community ------------------------------------------------------
        member _.TabCommunity = get "tab_community"
        member _.CommunitySection = get "community_section"
        member _.CommunityJoinTitle = get "community_join_title"
        member _.CommunityJoinHint = get "community_join_hint"
        member _.CommunityNamePlaceholder = get "community_name_placeholder"
        member _.BtnJoinCommunity = get "btn_join_community"
        member _.BtnRefresh = get "btn_refresh"
        member _.BtnCommunityGuides = get "btn_community_guides"
        member _.CommunityLoading = get "community_loading"
        member _.CommunityLoadingMore = get "community_loading_more"

        // ---- PULSE (chat) and the community toolbar ----------------------
        member _.PulseTitle = get "pulse_title"
        member _.PulseTagline = get "pulse_tagline"
        member _.PulsePlaceholder = get "pulse_placeholder"
        member _.PulseEmpty = get "pulse_empty"
        member _.PulseLoadingOlder = get "pulse_loading_older"
        member _.PulseImageExpired = get "pulse_image_expired"
        member _.PulseRules = get "pulse_rules"
        member _.PulseJoinFirst = get "pulse_join_first"
        member _.ChatReply = get "chat_reply"
        member _.ChatReplyingTo = get "chat_replying_to"
        member _.ChatSending = get "chat_sending"
        member _.ChatPhoto = get "chat_photo"
        member _.ChatSaveImage = get "chat_save_image"
        member _.ChatClose = get "chat_close"
        member _.ChatReact = get "chat_react"
        member _.BtnSend = get "btn_send"
        member _.BtnAttachImage = get "btn_attach_image"
        member _.BtnDelete = get "btn_delete"
        member _.CommunityTabGames = get "community_tab_games"


        // ---- LIVE FLOW ---------------------------------------------------
        // Supplied as one translated set and placed here under the app's own
        // key scheme, prefixed so none of it can collide with a name the rest
        // of the interface already owns.
        member _.LfProcessing = get "lf_processing"
        member _.LfDlss5 = get "lf_dlss_5"
        member _.LfLowLatency = get "lf_low_latency"
        member _.LfLowLatencyHint = get "lf_low_latency_hint"
        member _.LfLatencyMeasured = get "lf_latency_measured"
        member _.LfOff = get "lf_off"
        member _.LfOn = get "lf_on"
        member _.LfMultiPass = get "lf_multi_pass"
        member _.LfSharpness = get "lf_sharpness"
        member _.LfSharpnessHint = get "lf_sharpness_hint"
        member _.LfDenoiser = get "lf_denoiser"
        member _.LfDenoiserHint = get "lf_denoiser_hint"
        member _.LfUpscaler = get "lf_upscaler"
        member _.LfUpscalerHint = get "lf_upscaler_hint"
        member _.LfVrr = get "lf_vrr"
        member _.LfVrrHint = get "lf_vrr_hint"
        member _.LfVrrActive = get "lf_vrr_active"
        member _.LfVrrInactive = get "lf_vrr_inactive"
        member _.LfColors = get "lf_colors"
        member _.LfBrightness = get "lf_brightness"
        member _.LfContrast = get "lf_contrast"
        member _.LfSaturation = get "lf_saturation"
        member _.LfWarmth = get "lf_warmth"
        member _.LfColorsReset = get "lf_colors_reset"
        member _.LfMotionGpu = get "lf_motion_gpu"
        member _.LfMotionCpu = get "lf_motion_cpu"
        member _.LfEdgeSmoothing = get "lf_edge_smoothing"
        member _.LfEdgeSmoothingHint = get "lf_edge_smoothing_hint"
        member _.LfFlowMfg = get "lf_flow_mfg"
        member _.LfFlowMfgHint = get "lf_flow_mfg_hint"
        member _.LfFlowMode = get "lf_flow_mode"
        member _.LfFlowModeHint = get "lf_flow_mode_hint"
        member _.LfFgQueue = get "lf_fg_queue"
        member _.LfFgQueueHint = get "lf_fg_queue_hint"
        member _.LfFgQueue0 = get "lf_fg_queue_0"
        member _.LfFgQueue1 = get "lf_fg_queue_1"
        member _.LfFgQueue2 = get "lf_fg_queue_2"
        member _.LfSelectWindow = get "lf_select_window"
        member _.LfSelectCancel = get "lf_select_cancel"
        member _.LfSelectWindowHint = get "lf_select_window_hint"
        member _.LfMfgTitle = get "lf_mfg_title"
        member _.LfFgResolution = get "lf_fg_resolution"
        member _.LfFgResolutionHint = get "lf_fg_resolution_hint"
        member _.LfColorsHdr = get "lf_colors_hdr"
        member _.LfColorSpace = get "lf_color_space"
        member _.LfColorSpaceHint = get "lf_color_space_hint"
        member _.LfColorSpaceAuto = get "lf_color_space_auto"
        member _.LfHdrSwitchHint = get "lf_hdr_switch_hint"
        member _.LfFpsCounter = get "lf_fps_counter"
        member _.LfRecordSection = get "lf_record_section"
        member _.LfSmfgTitle = get "lf_smfg_title"
        member _.LfSmfgAdvanced = get "lf_smfg_advanced"
        member _.LfNrModel = get "lf_nr_model"
        member _.LfNrModelQuality = get "lf_nr_model_quality"
        member _.LfNrModelFast = get "lf_nr_model_fast"
        member _.LfNrModelHint = get "lf_nr_model_hint"
        member _.LfWindowPick = get "lf_window_pick"
        member _.LfWindowPickHint = get "lf_window_pick_hint"
        member _.LfSmfgMissing = get "lf_smfg_missing"
        member _.LfSmfgDlssPreset = get "lf_smfg_dlss_preset"
        member _.LfSmfgPresetDriver = get "lf_smfg_preset_driver"
        member _.LfSmfgPresetPerformance = get "lf_smfg_preset_performance"
        member _.LfSmfgPresetQuality = get "lf_smfg_preset_quality"
        member _.LfSmfgPresetBalanced = get "lf_smfg_preset_balanced"
        member _.LfSmfgPresetCWarning = get "lf_smfg_preset_c_warning"
        member _.LfSmfgGameFps = get "lf_smfg_game_fps"
        member _.LfSmfgGameFpsHelp = get "lf_smfg_game_fps_help"
        member _.LfSmfgGenerator = get "lf_smfg_generator"
        member _.LfSmfgMode = get "lf_smfg_mode"
        member _.LfSmfgGameType = get "lf_smfg_game_type"
        member _.LfSmfgAml = get "lf_smfg_aml"
        member _.LfSmfgUpscaler = get "lf_smfg_upscaler"
        member _.LfSmfgSharpness = get "lf_smfg_sharpness"
        member _.LfSmfgOverSharpness = get "lf_smfg_over_sharpness"
        member _.LfSmfgQueue = get "lf_smfg_queue"
        member _.LfSmfgScreenLimit = get "lf_smfg_screen_limit"
        member _.LfSmfgSync = get "lf_smfg_sync"
        member _.LfSmfgMotion = get "lf_smfg_motion"
        member _.LfSmfgStill = get "lf_smfg_still"
        member _.LfSmfgGuard = get "lf_smfg_guard"
        member _.LfSmfgGpu = get "lf_smfg_gpu"
        member _.LfSmfgPriority = get "lf_smfg_priority"
        member _.LfSmfgCapture = get "lf_smfg_capture"
        member _.LfSmfgShortcut = get "lf_smfg_shortcut"
        member _.LfSmfgEngine = get "lf_smfg_engine"
        member _.LfSmfgChecking = get "lf_smfg_checking"
        member _.LfSmfgRequired = get "lf_smfg_required"
        member _.LfSmfgUpdate = get "lf_smfg_update"
        member _.LfSmfgDownloading = get "lf_smfg_downloading"
        member _.LfSmfgFailed = get "lf_smfg_failed"
        member _.LfSmfgError = get "lf_smfg_error"
        member _.LfSmfgUnavailable = get "lf_smfg_unavailable"
        member _.LfSmfgStarting = get "lf_smfg_starting"
        member _.LfSmfgDownload = get "lf_smfg_download"
        member _.LfMsmLink = get "lf_msm_link"
        member _.LfMsmUnlink = get "lf_msm_unlink"
        member _.LfMsmDownload = get "lf_msm_download"
        member _.LfMsmOff = get "lf_msm_off"
        member _.LfMsmPick = get "lf_msm_pick"
        member _.LfMsmWaiting = get "lf_msm_waiting"
        member _.LfMsmLinked = get "lf_msm_linked"
        member _.LfShortcutShot = get "lf_shortcut_shot"
        member _.LfShortcutHintShot = get "lf_shortcut_hint_shot"
        member _.LfOverlaySection = get "lf_overlay_section"
        member _.LfKeepColors = get "lf_keep_colors"
        member _.LfKeepColorsHint = get "lf_keep_colors_hint"
        member _.LfLowLight = get "lf_low_light"
        member _.LfShortcutRecord = get "lf_shortcut_record"
        member _.LfShortcutHintRecord = get "lf_shortcut_hint_record"
        member _.LfLowLightHint = get "lf_low_light_hint"
        member _.LfFlowQuality = get "lf_flow_quality"
        member _.LfFlowPerformance = get "lf_flow_performance"
        member _.LfFrameLimit = get "lf_frame_limit"
        member _.LfFrameLimitHint = get "lf_frame_limit_hint"
        member _.LfStop = get "lf_stop"
        member _.LfStopHint = get "lf_stop_hint"
        member _.LfStart = get "lf_start"
        member _.LfStartHint = get "lf_start_hint"

        // ---- The community, seen from the Manage sheet -------------------
        member _.CommunityGlanceNone = get "community_glance_none"
        member _.CommunityGlanceWorked = get "community_glance_worked"
        member _.CommunityGlanceMixed = get "community_glance_mixed"
        member _.CommunityGlanceFailed = get "community_glance_failed"
        member _.CommunityGlanceOpen = get "community_glance_open"
        member _.CommunityGlanceHint = get "community_glance_hint"
        member _.CommunityGlanceRefresh = get "community_glance_refresh"
        member _.CommunityGlanceRefreshHint = get "community_glance_refresh_hint"
        member _.LfNetworkResolution = get "lf_network_resolution"
        member _.LfFast = get "lf_fast"
        member _.LfBalanced = get "lf_balanced"
        member _.LfQuality = get "lf_quality"
        member _.LfSource = get "lf_source"
        member _.LfRefresh = get "lf_refresh"
        member _.LfReconnect = get "lf_reconnect"
        member _.LfSelectAWindow = get "lf_select_a_window"
        member _.LfGoToApp = get "lf_go_to_app"
        member _.LfClear = get "lf_clear"
        member _.LfEffect = get "lf_effect"
        member _.LfProfile = get "lf_profile"
        member _.LfModel = get "lf_model"
        member _.LfIntensity = get "lf_intensity"
        member _.LfLocalTone = get "lf_local_tone"
        member _.LfLocalStructure = get "lf_local_structure"
        member _.LfSkinStructure = get "lf_skin_structure"
        member _.LfComparison = get "lf_comparison"
        member _.LfBeforeAfterWipe = get "lf_before_after_wipe"
        member _.LfMyPresets = get "lf_my_presets"
        member _.LfPresetName = get "lf_preset_name"
        member _.LfSavedPresets = get "lf_saved_presets"
        member _.LfNameYourPreset = get "lf_name_your_preset"
        member _.LfChooseAPreset = get "lf_choose_a_preset"
        member _.LfSaveNew = get "lf_save_new"
        member _.LfUpdateSelected = get "lf_update_selected"
        member _.LfApply = get "lf_apply"
        member _.LfDelete = get "lf_delete"
        member _.LfSettings = get "lf_settings"
        member _.LfProcessingGpu = get "lf_processing_gpu"
        member _.LfDisplay = get "lf_display"
        member _.LfMotionEstimation = get "lf_motion_estimation"
        member _.LfSkipStaticFrames = get "lf_skip_static_frames"
        member _.LfHdrCompatibility = get "lf_hdr_compatibility"
        member _.LfCapture = get "lf_capture"
        member _.LfMicrophone = get "lf_microphone"
        member _.LfRecordingRate = get "lf_recording_rate"
        member _.LfSystemAudio = get "lf_system_audio"
        member _.LfChooseFolder = get "lf_choose_folder"
        member _.LfScreenshot = get "lf_screenshot"
        member _.LfRecord = get "lf_record"
        member _.LfObsOutputSpout2 = get "lf_obs_output_spout2"
        member _.LfObsHelp = get "lf_obs_help"
        member _.LfGetObsSpout2Plugin = get "lf_get_obs_spout2_plugin"
        member _.LfNone = get "lf_none"
        member _.LfForCreators = get "lf_for_creators"
        member _.LfObsGuide = get "lf_obs_guide"
        member _.LfObsGuideTitle = get "lf_obs_guide_title"
        member _.LfObsSteps = get "lf_obs_steps"
        member _.LfShortcutOverlay = get "lf_shortcut_overlay"
        member _.LfRadeonTitle = get "lf_radeon_title"
        member _.LfRadeonNotice = get "lf_radeon_notice"
        member _.LfNvidiaOnly = get "lf_nvidia_only"
        member _.LfRadeonMotion = get "lf_radeon_motion"
        member _.LfFlowView = get "lf_flow_view"
        member _.LfFlowViewHint = get "lf_flow_view_hint"
        member _.LfViewMotion = get "lf_view_motion"
        member _.LfViewCorrected = get "lf_view_corrected"
        member _.LfViewHud = get "lf_view_hud"
        member _.LfViewPanel = get "lf_view_panel"
        member _.LfFpsOn = get "lf_fps_on"
        member _.LfFpsOff = get "lf_fps_off"
        member _.LfFpsBase = get "lf_fps_base"
        member _.LfFpsHint = get "lf_fps_hint"
        member _.LfShortcutHintOverlay = get "lf_shortcut_hint_overlay"
        member _.LfShortcutDlss = get "lf_shortcut_dlss"
        member _.LfShortcutHintDlss = get "lf_shortcut_hint_dlss"
        member _.LfShortcutListening = get "lf_shortcut_listening"
        member _.LfShortcutPresets = get "lf_shortcut_presets"
        member _.LfShortcutChange = get "lf_shortcut_change"
        member _.LfOpenOverlayNow = get "lf_open_overlay_now"
        // ---- The gallery -------------------------------------------------
        member _.GalleryTitle = get "gallery_title"
        member _.GalleryTagline = get "gallery_tagline"
        member _.GalleryWall = get "gallery_wall"
        member _.GalleryQueue = get "gallery_queue"
        member _.GalleryUpload = get "gallery_upload"
        member _.GalleryCaption = get "gallery_caption"
        member _.GalleryEmpty = get "gallery_empty"
        member _.GalleryQueueEmpty = get "gallery_queue_empty"
        member _.GalleryApprove = get "gallery_approve"
        member _.GalleryReject = get "gallery_reject"
        member _.GalleryPromote = get "gallery_promote"
        member _.GalleryModeration = get "gallery_moderation"
        member _.FilterRoute = get "filter_route"
        member _.FilterResult = get "filter_result"
        member _.FilterSort = get "filter_sort"
        member _.FilterClear = get "filter_clear"
        member _.SearchingFor = get "searching_for"
        member _.CommunityEmptyTitle = get "community_empty_title"
        member _.CommunityEmptyHint = get "community_empty_hint"
        member _.CommunityShare = get "community_share"
        member _.CommunityPostTitle = get "community_post_title"
        member _.CommunityResult = get "community_result"
        member _.CommunityInstallMethod = get "community_install_method"
        member _.CommunityNote = get "community_note"
        member _.CommunityNotePlaceholder = get "community_note_placeholder"
        member _.CommunitySpecs = get "community_specs"
        member _.BtnDetectSpecs = get "btn_detect_specs"
        member _.BtnRemoveSpecs = get "btn_remove_specs"
        member _.BtnPost = get "btn_post"
        member _.BtnReply = get "btn_reply"
        member _.ReplyPlaceholder = get "reply_placeholder"
        member _.NoReportsTitle = get "no_reports_title"
        member _.StatusWorking = get "status_working"
        member _.StatusMixed = get "status_mixed"
        member _.StatusBroken = get "status_broken"

        // ---- Features card -------------------------------------------------
        member _.FeaturesSection = get "features_section"
        member _.PerformanceMode = get "performance_mode"
        member _.AdvInstall = get "adv_install"
        member _.AdvInstallTip = get "adv_install_tip"
        member _.AdvBadge = get "adv_badge"
        member _.AdvAutomatic = get "adv_automatic"
        member _.AdvAutomaticHint = get "adv_automatic_hint"
        member _.AdvBuild = get "adv_build"
        member _.AdvCustom = get "adv_custom"
        member _.AdvCustomTip = get "adv_custom_tip"
        member _.AdvGroupNvidia = get "adv_group_nvidia"
        member _.AdvGroupYours = get "adv_group_yours"
        member _.AdvApply = get "adv_apply"
        member _.AdvNoChanges = get "adv_no_changes"
        member _.AdvApplyTip = get "adv_apply_tip"
        member _.AdvRemoveAll = get "adv_remove_all"
        member _.AdvRemoveAllTip = get "adv_remove_all_tip"
        member _.AdvGroupReShade = get "adv_group_reshade"
        member _.AdvGroupOptiScaler = get "adv_group_optiscaler"
        member _.AdvGroupRenoDx = get "adv_group_renodx"
        member _.AdvGroupDeepFried = get "adv_group_deepfried"
        member _.AdvGroupDx9 = get "adv_group_dx9"
        member _.AdvGroupAmd = get "adv_group_amd"
        member _.AdvGroupDlss5 = get "adv_group_dlss5"
        member _.AdvAddExtraFile = get "adv_add_extra_file"
        member _.AdvAddExtraFolder = get "adv_add_extra_folder"
        member _.AdvAddExtraTip = get "adv_add_extra_tip"
        member _.AdvShareExtraTip = get "adv_share_extra_tip"
        member _.MinimizeToTray = get "minimize_to_tray"
        member _.MinimizeToTrayTip = get "minimize_to_tray_tip"
        member _.StartWithWindows = get "start_with_windows"
        member _.StartWithWindowsTip = get "start_with_windows_tip"
        member _.TrayOpen = get "tray_open"
        member _.TrayQuit = get "tray_quit"
        member _.ModeAdvanced = get "mode_advanced"
        member _.ModeEasy = get "mode_easy"
        member _.ModeAdvancedTip = get "mode_advanced_tip"
        member _.ModeEasyTip = get "mode_easy_tip"
        member _.ModeEasyLocked = get "mode_easy_locked"
        member _.ModeEasyPending = get "mode_easy_pending"
        member _.AdvAddAddon = get "adv_add_addon"
        member _.AdvAddPlugin = get "adv_add_plugin"
        member _.AdvAddAddonTip = get "adv_add_addon_tip"
        member _.AdvAddPluginTip = get "adv_add_plugin_tip"
        member _.AdvRemoveFile = get "adv_remove_file"
        member _.AdvShareParts = get "adv_share_parts"
        member _.AdvShareOnlyWorking = get "adv_share_only_working"
        member _.AdvConflictTitle = get "adv_conflict_title"
        member _.AdvConflictText = get "adv_conflict_text"
        member _.AdvFilesOnGame = get "adv_files_on_game"
        member _.AdvOptAuto = get "adv_opt_auto"
        member _.AdvOptStandard = get "adv_opt_standard"
        member _.AdvWhereGame = get "adv_where_game"
        member _.AdvWhereHost64 = get "adv_where_host64"
        member _.AdvStatusInstalled = get "adv_status_installed"
        member _.AdvStatusNotInstalled = get "adv_status_not_installed"
        member _.AdvStatusWillInstall = get "adv_status_will_install"
        member _.AdvStatusWillReinstall = get "adv_status_will_reinstall"
        member _.AdvStatusWillRepair = get "adv_status_will_repair"
        member _.AdvStatusWillRemove = get "adv_status_will_remove"
        member _.AdvStatusPresent = get "adv_status_present"
        member _.AdvStatusUpToDate = get "adv_status_uptodate"
        member _.AdvStatusNoStreamline = get "adv_status_nostreamline"
        member _.AdvStatusNoFolders = get "adv_status_nofolders"
        member _.AdvStatusNoExtras = get "adv_status_noextras"
        member _.AdvStatusMissing = get "adv_status_missing"
        member _.AdvStatusDownload = get "adv_status_download"
        member _.AdvStatusChecking = get "adv_status_checking"
        /// "Apply 3 change(s)" with the count filled in.
        member _.AdvApplyCount(count: int) = (get "adv_apply_count").Replace("{count}", string count)
        /// A part of the advanced install by its id ("reshade", "host64", ...): its name and what it is.
        member _.AdvPartName(id: string) = get ("adv_part_" + id)
        member _.AdvPartDesc(id: string) = get ("adv_part_" + id + "_desc")
        member _.PerformanceTagline = get "performance_tagline"
        member _.AmdTagline = get "amd_tagline"
        member _.SupportTagline = get "support_tagline"

        // ---- Dynamic Overlay ------------------------------------------------
        member _.OverlaySection = get "overlay_section"
        member _.OverlayTitle = get "overlay_title"
        member _.OverlayTagline = get "overlay_tagline"
        member _.OverlayDesc = get "overlay_desc"
        member _.OverlayStyle = get "overlay_style"
        member _.OverlayHotkey = get "overlay_hotkey"
        member _.OverlayHotkeyDesc = get "overlay_hotkey_desc"
        member _.OptiMenuKey = get "opti_menu_key"
        member _.OptiMenuKeyTitle = get "opti_menu_key_title"
        member _.OptiMenuKeyDesc = get "opti_menu_key_desc"
        member _.Extras = get "extras"
        member _.ExtrasDesc = get "extras_desc"
        member _.BtnAddExtra = get "btn_add_extra"

        // ---- Downloads sheet (CloudAssets) ----------------------------------
        member _.AssetsTitle = get "assets_title"
        member _.AssetsSubtitle = get "assets_subtitle"
        member _.AssetsButtonTip = get "assets_button_tip"
        member _.AssetsAllReady = get "assets_all_ready"
        member _.AssetsDownloadAll = get "assets_download_all"
        member _.AssetsLater = get "assets_later"
        member _.AssetsDone = get "assets_done"
        member _.AssetOverlayBlurb = get "asset_overlay_blurb"
        member _.AssetXessBlurb = get "asset_xess_blurb"
        member _.AssetNotDownloaded = get "asset_not_downloaded"
        member _.AssetReady = get "asset_ready"
        member _.AssetStarting = get "asset_starting"
        member _.AssetFailed = get "asset_failed"
        member _.AssetMisplaced = get "asset_misplaced"
        member _.AssetDownload = get "asset_download"
        member _.AssetDownloading = get "asset_downloading"
        member _.AssetRetry = get "asset_retry"
        member _.OverlayLocked = get "overlay_locked"
        member _.AntiCheatTitle = get "anti_cheat_title"
        member _.AntiCheatWarning = get "anti_cheat_warning"
        member _.OverlayLockedDesc = get "overlay_locked_desc"

        // ---- Manage sheet: key bindings --------------------------------------
        member _.KeysSection = get "keys_section"
        member _.KeysSectionDesc = get "keys_section_desc"
        member _.DllNameSection = get "dll_name_section"
        member _.DllNameToggle = get "dll_name_toggle"
        member _.DllNameDesc = get "dll_name_desc"
        member _.DllNameCustom = get "dll_name_custom"
        member _.DllNameApply = get "dll_name_apply"
        member _.DllNameCurrent = get "dll_name_current"
        member _.DllNameInstallFirst = get "dll_name_install_first"
        member _.DllNamePlaceholder = get "dll_name_placeholder"
        member _.DllNameTaken = get "dll_name_taken"
        member _.DownloadsHintTitle = get "downloads_hint_title"
        member _.DownloadsHintText = get "downloads_hint_text"
        member _.ManageRescanTip = get "manage_rescan_tip"
        member _.ManageInstallingTip = get "manage_installing_tip"
        member _.ManageShowDetails = get "manage_show_details"
        member _.ManageDlss5Status = get "manage_dlss5_status"
        member _.ManageAskingCommunity = get "manage_asking_community"
        member _.ManageShipsDlssTip = get "manage_ships_dlss_tip"
        member _.RouteRecommendedTip = get "route_recommended_tip"
        member _.RouteDx12Tip = get "route_dx12_tip"
        member _.RouteDx11Tip = get "route_dx11_tip"
        member _.RouteDx9Tip = get "route_dx9_tip"
        member _.RouteVulkanTip = get "route_vulkan_tip"
        member _.OptiDx12Tip = get "opti_dx12_tip"
        member _.OptiVulkanTip = get "opti_vulkan_tip"
        member _.Arch32Tip = get "arch_32_tip"
        member _.AmdModeNotice = get "amd_mode_notice"
        member _.TipNeural = get "tip_neural"
        member _.TipMfgUnlock = get "tip_mfgunlock"
        member _.TipMultipass = get "tip_multipass"
        member _.TipDeepFried = get "tip_deepfried"
        member _.TipDeepFriedVulkan = get "tip_deepfried_vulkan"
        member _.TipDeepFriedHost64 = get "tip_deepfried_host64"
        member _.TipOverlay = get "tip_overlay"
        member _.EmuDxTip = get "emu_dx_tip"
        member _.EmuVulkanTip = get "emu_vulkan_tip"
        member _.EmuApiHint = get "emu_api_hint"
        member _.ChangeExeTip = get "change_exe_tip"
        member _.KeyOpenReShadeWith = get "key_open_reshade_with"
        member _.KeyOpenOverlayWith = get "key_open_overlay_with"
        member _.HintRouteOptiScaler = get "hint_route_optiscaler"
        member _.HintRouteRenoDx = get "hint_route_renodx"
        member _.HintRouteVulkan = get "hint_route_vulkan"
        member _.HintRouteDx9 = get "hint_route_dx9"
        member _.HintRouteEmulator = get "hint_route_emulator"
        member _.HintRouteAmd = get "hint_route_amd"
        member _.DllNameVulkanLayer = get "dll_name_vulkan_layer"
        member _.KeyOptiMenu = get "key_opti_menu"
        member _.KeyReShade = get "key_reshade"
        member _.KeyOverlay = get "key_overlay"
        member _.KeyChange = get "key_change"

        // ---- Settings: background ---------------------------------------------
        member _.BackgroundBrightness = get "bg_brightness"
        member _.BackgroundBrightnessDesc = get "bg_brightness_desc"
        member _.BackgroundCustom = get "bg_custom"
        member _.BackgroundCustomPick = get "bg_custom_pick"
        member _.BackgroundCustomRemove = get "bg_custom_remove"

        // ---- Theme names --------------------------------------------------
        /// Same order as `MainViewModel.atmosphereKeys`; the English key is what
        /// gets saved and matched, this is only what the user reads.
        member _.AtmosphereNames =
            [| get "theme_neon_emerald"
               get "theme_obsidian_onyx"
               get "theme_supernova_flare"
               get "theme_cyber_nebula"
               get "theme_emerald_horizon"
               get "theme_midnight_titanium"
               get "theme_frost_glacier"
               get "theme_eclipse_crimson"
               get "theme_deep_astral" |]

        /// Same order as `MainViewModel.GeometricMotifs`.
        member _.MotifNames =
            [| get "theme_orbital_spheres"
               get "theme_prism_auroras"
               get "theme_floating_crystals"
               get "theme_stardust_particles"
               get "theme_cyber_flux"
               get "theme_minimal_clean" |]

        /// Same order as `ModInstaller.overlayThemes`. The English name is what
        /// is saved and what the in-game add-on reads; this is only what the
        /// style picker shows.
        member _.OverlayThemeNames =
            [| get "overlay_theme_neon_emerald"
               get "overlay_theme_cyber_cyan"
               get "overlay_theme_electric_violet"
               get "overlay_theme_supernova_amber"
               get "overlay_theme_eclipse_crimson"
               get "overlay_theme_graphite_minimal" |]

        // ---- Community toolbar dropdowns ----------------------------------
        /// Same order as `CommunityFilters.routeKeys`. OptiScaler, ReShade and
        /// AMD are names and read the same in every language.
        member _.CommunityRouteOptions =
            [| get "filter_route_all"; "OptiScaler"; "ReShade"; get "route_emulator"; "AMD" |]

        /// Same order as `CommunityFilters.resultKeys`.
        member _.CommunityResultOptions =
            [| get "filter_result_any"; get "result_working"; get "result_mixed"; get "result_broken" |]

        /// Same order as `CommunityFilters.sortKeys`.
        member _.CommunitySortOptions =
            [| get "sort_recent"; get "sort_reports"; get "sort_title" |]

        // ---- Counters ------------------------------------------------------
        /// "TOTAL GAMES: {count}" with the placeholder filled in.
        member _.TotalGames(count: int) =
            (get "total_games").Replace("{count}", string count)

        member _.StatusGamesReady(count: int) =
            (get "status_games_ready").Replace("{count}", string count)

    /// The strings the whole app is showing right now.
    ///
    /// MainViewModel owns the language and writes this whenever it changes, so
    /// a view that is not bound to MainViewModel - LIVE FLOW and its overlay,
    /// which carry their own view-model - can read the same set without it
    /// being threaded through every constructor.
    let mutable current = Strings("en")

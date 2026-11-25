using System.Text;
using CATHODE;

namespace AIMP.Launcher;

/// <summary>
/// Third-person clips made for AnimationSync. Each one is authored in Blender
/// on the MALE rig and built once with OpenCAGE (import_animation with
/// stored_path ANIMATION\AIMP\&lt;ALIAS&gt;). That import writes exactly: the
/// two streamed sections ANIM_CLIP_DB_SEC_&lt;path&gt;.BIN (32/64-bit), a global
/// section entry path -&gt; (path, -1), one alias in the set's default table and
/// three debug names (path, alias, label "&lt;SET&gt;\&lt;ALIAS&gt;" of the set it was
/// imported into); OpenCAGE resolves every clip's names when it loads the PAK
/// and crashes on a missing one, the game itself never reads them.
/// The launcher ships the two sections (Resources\AnimationSync) and replays
/// the three edits for all five representation sets, so every player gets the
/// clips without OpenCAGE. AIMP.Native AnimationSyncThirdPerson.h (Tier C)
/// maps the owner's first-person alias to these aliases.
/// </summary>
internal static partial class RemoteCrouchAnimationInstaller
{
    internal sealed record ThirdPersonClip(string Alias, string StoredPath, string Label)
    {
        internal uint AliasHash => AnimClipDatabase.HashName(Alias);
        internal uint PathHash => AnimClipDatabase.HashName(StoredPath);
    }

    internal static readonly IReadOnlyList<ThirdPersonClip> ThirdPersonClips =
    [
        Aimp("lever_2_use_3p"),
        Aimp("cut_panel_fp_cut_3p"),
        Aimp("cut_panel_fp_enter_3p"),
        Aimp("cut_panel_fp_pull_lever_3p"),
        Aimp("cut_panel_fp_put_cutter_away_3p"),
        Aimp("cut_push_out_vent_panel_3p"),
        Aimp("hacking_tool_confirm_3p"),
        Aimp("hacking_tool_intro_3p"),
        Aimp("hacking_tool_loop_3p"),
        Aimp("hacking_tool_nudge1_3p"),
        Aimp("hacking_tool_nudge2_3p"),
        Aimp("hacking_tool_nudge3_3p"),
        Aimp("hacking_tool_outro_3p"),
        Aimp("hacking_tool_twist_switch_3p"),
        Aimp("interact_keycard_stand_add_3p"),
        Aimp("lift_button_press_3p"),
        Aimp("push_button_01_3p"),
        Aimp("rewire_box_close_player_3p"),
        Aimp("rewire_box_open_player_3p"),
        Aimp("rewire_box_use_player_3p"),
        Aimp("use_terminal_breathe_3p"),
        Aimp("use_terminal_exit_3p"),
        Aimp("use_terminal_intro_3p"),
        Aimp("player_hiding_draggedout_android_3p"),
        Aimp("player_hiding_high_enter_3p"),
        Aimp("player_hiding_high_exit_3p"),
        Aimp("player_hiding_high_idle_3p"),
        Aimp("player_hiding_high_torrens_3p"),
        Aimp("player_hiding_low_draggedout_android_3p"),
        Aimp("player_hiding_low_enter_3p"),
        Aimp("player_hiding_low_exit_3p"),
        Aimp("player_hiding_low_idle_3p"),
        Aimp("kill_fp_floor_vent_grab_001_3p"),
        Aimp("kill_fp_hiding_locker_001_3p"),
        Aimp("kill_fp_hiding_locker_low_001_3p"),
        Aimp("kill_fp_hiding_locker_low_002_3p"),
        Aimp("kill_fp_hiding_locker_low_003_3p"),
        Aimp("kill_fp_hiding_locker_low_004_3p"),
        Aimp("kill_fp_wall_vent_grab_back_001_3p"),
        Aimp("kill_fp_wall_vent_grab_back_002_3p"),
        Aimp("kill_fp_wall_vent_grab_back_003_3p"),
        Batch4("cut_panel_fp_exit_3p"),
        Batch4("cut_panel_fp_put_cutter_away_fast_3p"),
        Batch4("cut_panel_trans_to_panel_3p"),
        Batch4("cut_vent_panel_3p"),
        Batch4("cut_vent_trans_to_vent_3p"),
        Batch4("cutterupgradecanister_3p"),
        Batch4("cutting_tool_pickup_3p"),
        Batch4("deep_crouch_fail_attach_3p"),
        Batch4("door_brace_end_3p"),
        Batch4("door_brace_full_3p"),
        Batch4("door_brace_loop_3p"),
        Batch4("door_brace_start_3p"),
        Batch4("fp_flame_pickup_3p"),
        Batch4("fp_flame_pickup_lower_3p"),
        Batch4("fp_radiation_vent_entry_3p"),
        Batch4("hacking_tool_nudge_down_3p"),
        Batch4("hacking_tool_nudge_left_3p"),
        Batch4("hacking_tool_nudge_right_3p"),
        Batch4("hacking_tool_nudge_up_3p"),
        Batch4("hacking_tool_twist_switch_scrub_3p"),
        Batch4("hackingupgradecap_3p"),
        Batch4("keyboard_generic_use_01_3p"),
        Batch4("m21_internal_lift_hatch_climb_3p"),
        Batch4("m21_lift_shaft_leap_3p"),
        Batch4("m21_piperoom_end_3p"),
        Batch4("m21_piperoom_end_mantle_3p"),
        Batch4("m21_piperoom_mid_mantle_3p"),
        Batch4("m21_piperoom_start_3p"),
        Batch4("melee_ground_01_p1_3p"),
        Batch4("melee_ground_01_p2_3p"),
        Batch4("open_vent_from_inside_3p"),
        Batch4("player_floor_vent_extract_b_3p"),
        Batch4("player_floor_vent_extract_f_3p"),
        Batch4("player_wall_vent_extract_b_3p"),
        Batch4("player_wall_vent_extract_f_3p"),
        Batch4("portable_computer_use_01_3p"),
        Batch4("replace_lift_component_3p"),
        Batch4("sd_arm_botleft_3p"),
        Batch4("sd_arm_botright_3p"),
        Batch4("sd_arm_breathe_3p"),
        Batch4("sd_arm_exit_3p"),
        Batch4("sd_arm_intro_3p"),
        Batch4("sd_arm_intro_open_3p"),
        Batch4("sd_arm_topleft_3p"),
        Batch4("sd_arm_topright_3p"),
        Batch4("tracker_pickup_3p"),
        Batch4("trav_fp_leap_4m_3p"),
        Batch4("trav_sidle_idle_3p"),
        Batch4("trav_sidle_in_3p"),
        Batch4("trav_sidle_move_l_3p"),
        Batch4("trav_sidle_move_r_3p"),
        Batch4("trav_sidle_out_3p"),
        Batch4("trunction_activate_3p"),
    ];

    // Built by OpenCAGE into PARKER with stored_path ANIMATION\AIMP\<ALIAS>; its label is PARKER\<ALIAS>.
    private static ThirdPersonClip Aimp(string alias) =>
        new(alias, @"ANIMATION\AIMP\" + alias.ToUpperInvariant(), @"PARKER\" + alias.ToUpperInvariant());

    // Built in one multi-clip OpenCAGE import (batch4_all_3p.fbx) into PARKER: default stored paths.
    private static ThirdPersonClip Batch4(string alias) =>
        new(alias, @"ANIMATION\OPENCAGE\PARKER\BATCH4_ALL_3P_" + alias.ToUpperInvariant(), @"PARKER\" + alias.ToUpperInvariant());

    private static readonly string[] SectionDirectories = ["STREAMED", "STREAMED64"];
    internal const string DebugStringsEntry = @"DATA\ANIM_SYS\ANIM_STRING_DB_DEBUG.BIN";

    internal static IEnumerable<string> DebugNames(ThirdPersonClip clip) => [clip.StoredPath, clip.Alias, clip.Label];

    // Identity of everything this part writes: rows and section bytes.
    internal static string ThirdPersonClipsSha256()
    {
        using var hash = System.Security.Cryptography.IncrementalHash.CreateHash(System.Security.Cryptography.HashAlgorithmName.SHA256);
        foreach (var clip in ThirdPersonClips)
        {
            hash.AppendData(Encoding.UTF8.GetBytes(clip.ToString()));
            foreach (var directory in SectionDirectories) hash.AppendData(ThirdPersonSection(clip, directory));
        }
        return Convert.ToHexString(hash.GetHashAndReset());
    }

    internal static string StreamedSectionEntry(string directory, uint path) =>
        $@"DATA\ANIM_SYS\{directory}\ANIM_CLIP_DB_SEC_{path}.BIN";

    internal static byte[] ThirdPersonSection(ThirdPersonClip clip, string directory)
    {
        var name = $"AIMP.Launcher.AnimationSync.{clip.Alias}.{directory.ToLowerInvariant()}.bin";
        using var stream = typeof(RemoteCrouchAnimationInstaller).Assembly.GetManifestResourceStream(name)
            ?? throw new InvalidDataException($"The launcher is missing the animation resource {name}.");
        using var memory = new MemoryStream();
        stream.CopyTo(memory);
        return memory.ToArray();
    }

    // Runs on the retail-based PAK after the crouch patch; idempotent. Retail
    // never holds these paths or aliases: a different value is refused.
    private static void PatchThirdPersonClips(PAK2 pak)
    {
        var globalEntry = Entry(pak, GlobalClipDbEntry);
        var global = GlobalClipDatabase.Parse(globalEntry.Content);
        foreach (var clip in ThirdPersonClips)
        {
            DeclareStreamed(global, clip);
            foreach (var directory in SectionDirectories) AddSection(pak, clip, directory);
        }
        globalEntry.Content = global.Serialize();
        foreach (var profile in Profiles) AddAliases(pak, profile);
        AddDebugNames(Entry(pak, DebugStringsEntry));
    }

    // [n][n][(hash, index) x n][offset x n][NUL-terminated names]: the clip
    // database table layout. New names go at the end, as OpenCAGE writes them.
    private static void AddDebugNames(PAK2.File entry)
    {
        using var reader = new BinaryReader(new MemoryStream(entry.Content));
        var table = AnimClipDatabase.ReadTable(reader, r => r.ReadInt32());
        var names = new MemoryStream();
        reader.BaseStream.CopyTo(names);
        foreach (var text in ThirdPersonClips.SelectMany(DebugNames))
        {
            if (table.Contains(AnimClipDatabase.HashName(text))) continue;
            table.Set(AnimClipDatabase.HashName(text), checked((int)names.Length));
            names.Write(Encoding.ASCII.GetBytes(text + "\0"));
        }
        using var stream = new MemoryStream();
        using (var writer = new BinaryWriter(stream, Encoding.ASCII, true)) AnimClipDatabase.WriteTable(writer, table, (w, v) => w.Write(v));
        stream.Write(names.ToArray());
        entry.Content = stream.ToArray();
    }

    private static string? DebugName(byte[] strings, uint hash)
    {
        using var reader = new BinaryReader(new MemoryStream(strings));
        var table = AnimClipDatabase.ReadTable(reader, r => r.ReadInt32());
        if (table.Get(hash) is not int offset) return null;
        var start = checked((int)reader.BaseStream.Position + offset);
        var end = Array.IndexOf(strings, (byte)0, start);
        return end < 0 ? null : Encoding.ASCII.GetString(strings, start, end - start);
    }

    private static void DeclareStreamed(GlobalClipDatabase global, ThirdPersonClip clip)
    {
        if (global.SectionGroup(clip.PathHash) is null) global.AddStreamedSection(clip.PathHash);
        else if (global.SectionGroup(clip.PathHash) != clip.PathHash || global.SectionClipIndex(clip.PathHash) != uint.MaxValue)
            throw new InvalidDataException($"ANIMATION.PAK already uses the clip path of '{clip.Alias}'.");
    }

    // OpenCAGE appends imported sections at the end of the archive; so do we.
    private static void AddSection(PAK2 pak, ThirdPersonClip clip, string directory)
    {
        var name = StreamedSectionEntry(directory, clip.PathHash);
        var section = ThirdPersonSection(clip, directory);
        var existing = pak.Entries.FirstOrDefault(e => e.Filename.Equals(name, StringComparison.OrdinalIgnoreCase));
        if (existing is null) pak.Entries.Add(new PAK2.File { Filename = name, Content = section });
        else if (!existing.Content.AsSpan().SequenceEqual(section))
            throw new InvalidDataException($"ANIMATION.PAK already contains a different {name}.");
    }

    private static void AddAliases(PAK2 pak, Profile profile)
    {
        var entry = Entry(pak, profile.ClipDbEntry);
        var db = ParseExact(entry.Content, profile.Character);
        foreach (var clip in ThirdPersonClips)
        {
            var wanted = new AnimClipDatabase.Clip(clip.PathHash, 0);
            var current = db.Animations.Get(clip.AliasHash);
            if (current is null) db.Animations.Set(clip.AliasHash, wanted);
            else if (current != wanted)
                throw new InvalidDataException($"{profile.Character} already has a different '{clip.Alias}' clip.");
        }
        entry.Content = db.Serialize();
    }

    private static void ValidateThirdPersonClips(PAK2 pak)
    {
        var global = GlobalClipDatabase.Parse(Entry(pak, GlobalClipDbEntry).Content);
        foreach (var clip in ThirdPersonClips)
        {
            if (global.SectionGroup(clip.PathHash) != clip.PathHash || global.SectionClipIndex(clip.PathHash) != uint.MaxValue)
                throw new InvalidDataException($"'{clip.Alias}' is not declared as a streamed clip.");
            foreach (var directory in SectionDirectories)
                if (!Entry(pak, StreamedSectionEntry(directory, clip.PathHash)).Content.AsSpan()
                        .SequenceEqual(ThirdPersonSection(clip, directory)))
                    throw new InvalidDataException($"'{clip.Alias}' {directory} section differs from the launcher's.");
            ValidateAliasesAndNames(pak, clip);
        }
    }

    private static void ValidateAliasesAndNames(PAK2 pak, ThirdPersonClip clip)
    {
        foreach (var profile in Profiles)
            if (AnimClipDatabase.Parse(Entry(pak, profile.ClipDbEntry).Content).Animations.Get(clip.AliasHash)
                != new AnimClipDatabase.Clip(clip.PathHash, 0))
                throw new InvalidDataException($"{profile.Character} cannot play '{clip.Alias}'.");
        var strings = Entry(pak, DebugStringsEntry).Content;
        if (DebugNames(clip).Any(name => DebugName(strings, AnimClipDatabase.HashName(name)) != name))
            throw new InvalidDataException($"'{clip.Alias}' lacks its debug names (OpenCAGE could not load the PAK).");
    }
}

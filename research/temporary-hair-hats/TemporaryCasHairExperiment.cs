// TEMPORARY RESEARCH ONLY. Never include in ApexRadiance.asi or final release.
// Reconstructed from the player's original UI.dll. Opt-in and IL-guarded.
// WARNING: Simulator.Sleep(0) in CAS has not yet been proven safe.
using System;
using System.Reflection;
using System.Runtime.InteropServices;
using MonoPatcherLib;
using Sims3.SimIFace;
using Sims3.SimIFace.CAS;
using Sims3.UI;
using Sims3.UI.CAS;

namespace ApexHairTemporaryResearch
{
    [Plugin(false)]
    public sealed class TemporaryCasHairExperiment
    {
        private const int ExpectedILLength = 292;
        private const ulong ExpectedFnv64 = 0x931FAD6E160F56CBUL;
        private static FieldInfo sFilter;
        private static MethodInfo sGetPartName;
        private static ConstructorInfo sThumbnailCtor;

        [DllImport("kernel32.dll", EntryPoint = "GetFileAttributesW", CharSet = CharSet.Unicode)]
        private static extern uint GetFileAttributesW(string path);

        public TemporaryCasHairExperiment()
        {
            // No game mscorlib Environment.GetEnvironmentVariable API.
            // Win32 marker file is opt-in, and avoids changing the user's
            // normal game environment or installing another library.
            if (GetFileAttributesW(
                    @"MonoPatcher\EnableApexHairResearch.txt") == 0xFFFFFFFFu)
                return;
            try
            {
                Type originalType = typeof(CASHair);
                BindingFlags privateInstance = BindingFlags.NonPublic | BindingFlags.Instance;
                MethodInfo original = originalType.GetMethod("AddHairTypeGridItem", privateInstance);
                sFilter = originalType.GetField("mContentTypeFilter", privateInstance);
                sGetPartName = originalType.GetMethod("GetPartName", privateInstance);
                // Official 1.69 UI.dll invokes the five-parameter ThumbnailKey
                // constructor with ResourceKey/int/uint/uint/ThumbnailSize.
                // MonoPatcher public reference DLLs may expose a different
                // overload: inspect the installed game, never trust that ABI.
                sThumbnailCtor = typeof(ThumbnailKey).GetConstructor(new Type[] {
                    typeof(ResourceKey), typeof(int), typeof(uint),
                    typeof(uint), typeof(ThumbnailSize) });
                if (original == null || sFilter == null || sGetPartName == null ||
                    sThumbnailCtor == null)
                    return;
                ParameterInfo[] args = original.GetParameters();
                if (args.Length != 5 ||
                    args[0].ParameterType != typeof(ItemGrid) ||
                    args[1].ParameterType != typeof(ResourceKey) ||
                    args[2].ParameterType != typeof(CASPartPreset) ||
                    args[3].ParameterType != typeof(bool) ||
                    args[4].ParameterType != typeof(bool).MakeByRefType() ||
                    original.ReturnType != typeof(bool) ||
                    original.MetadataToken != 0x0600191B)
                    return;
                MethodBody body = original.GetMethodBody();
                if (body == null) return;
                byte[] il = body.GetILAsByteArray();
                if (il == null || il.Length != ExpectedILLength ||
                    Fnv1a(il) != ExpectedFnv64)
                    return;
                MonoPatcher.PatchAll(typeof(TemporaryCasHairExperiment).Assembly);
            }
            catch
            {
                // Incompatible runtime/assembly: never force a replacement.
            }
        }

        private static ulong Fnv1a(byte[] data)
        {
            ulong h = 0xCBF29CE484222325UL;
            for (int i = 0; i < data.Length; ++i)
            {
                h ^= data[i];
                h = unchecked(h * 0x100000001B3UL);
            }
            return h;
        }

        [ReplaceMethod(typeof(CASHair), "AddHairTypeGridItem")]
        private bool AppendHairItemTimesliced(
            ItemGrid grid, ResourceKey layoutKey, CASPartPreset preset,
            bool inActiveWardrobe, ref bool filterFlag)
        {
            // MonoPatcher swaps this replacement's code onto an existing
            // CASHair instance, following its documented instance-patch model.
            CASHair owner = (CASHair)(this as object);
            Layout layout = UIManager.LoadLayout(layoutKey);
            WindowBase root = layout.GetWindowByExportID(1);
            if (root != null)
            {
                CustomContentIcon icon =
                    root.GetChildByID(23, true) as CustomContentIcon;
                icon.ContentType = UIUtils.GetCustomContentType(
                    preset.mPart.Key, preset.mPresetId);
                CatalogProductFilter filter =
                    (CatalogProductFilter)sFilter.GetValue(owner);
                if (filter.ObjectMatchesFilter(preset, ref filterFlag))
                {
                    Window preview = root.GetChildByID(20, true) as Window;
                    if (preview != null)
                    {
                        ImageDrawable image = preview.Drawable as ImageDrawable;
                        if (image != null)
                        {
                            ThumbnailKey key = (ThumbnailKey)sThumbnailCtor.Invoke(
                                new object[] { preset.mPart.Key,
                                    unchecked((int)preset.mPresetId),
                                    unchecked((uint)preset.mPart.BodyType),
                                    unchecked((uint)preset.mPart.AgeGenderSpecies),
                                    (ThumbnailSize)2 });
                            image.Image = UIManager.GetCASThumbnailImage(key);
                            preview.Invalidate();
                        }
                    }
                    if (inActiveWardrobe)
                    {
                        Window badge = root.GetChildByID(29, true) as Window;
                        if (badge != null) badge.Visible = true;
                    }
                    if (CASController.Singleton.DebugTooltips)
                        root.TooltipText = (string)sGetPartName.Invoke(
                            owner, new object[] { preset.mPart });
                    grid.AddItem(new ItemGridCellItem(root, preset));

                    // Only experimental change: yield AFTER a successful
                    // row insertion, preserving the original Boolean result.
                    // Safe simulator-task context must be proven in-game.
                    Simulator.Sleep(0);
                    return true;
                }
            }
            return false;
        }
    }
}

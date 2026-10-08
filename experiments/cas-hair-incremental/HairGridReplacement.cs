// Experimental, opt-in managed UI patch for the TS3 hair/hat catalogue.
// Not a replacement UI.dll and not enabled by the ApexRadiance.asi performance toggle.
// Experimental implementation for @idavidveiga's Apex fork; follows the repository license.
// API behavior cross-checked against original UI.dll metadata and public NRaas CAS references.
//
// Requires MonoPatcher by LazyDuchess for method replacement, and references to the user's own EA assemblies.
// Target: Sims3.UI.CAS.CASHair.PopulateTypesGrid(bool) in the supplied original UI.dll.
// The IL SHA-256 gate intentionally prevents patching unknown UI.dll builds and avoids conflicts with core replacements.
//
// This is an isolated research implementation, NOT a released/tested game mod.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Security.Cryptography;
using System.Threading;
using MonoPatcherLib;
using Sims3.SimIFace;
using Sims3.SimIFace.CAS;
using Sims3.UI;
using Sims3.UI.CAS;
using Sims3.UI.Store;

namespace VeigaApexCasHairExperimental
{
    [Plugin(false)] // Never patch automatically: constructor checks the opt-in marker and exact game method.
    public sealed class EntryPoint
    {
        private const string MethodSha256 =
            "e0866c4327c0eec9ae55485aee7601e6168aa651b0f201e168c440dadc10bd15";

        public EntryPoint()
        {
            try
            {
                // Installing this S3SA is NOT enough to change the game. The file is an explicit developer opt-in.
                string optIn = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                    @"Electronic Arts\The Sims 3\Apex Radiance\enable-experimental-hair-grid.txt");
                if (!File.Exists(optIn)) return;

                MethodInfo original = typeof(CASHair).GetMethod(
                    "PopulateTypesGrid",
                    BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                    null, new Type[] { typeof(bool) }, null);
                if (original == null || original.ReturnType != typeof(void) || original.GetMethodBody() == null)
                    return;

                byte[] il = original.GetMethodBody().GetILAsByteArray();
                if (il == null || !Sha256Equals(il, MethodSha256))
                {
                    Diagnostics.Write("UI.dll method fingerprint differs; no replacement installed");
                    return; // Unknown/modified UI.dll: deliberately fail closed.
                }

                MonoPatcher.PatchAll(typeof(EntryPoint).Assembly);
                Diagnostics.Write("Managed hair-grid patch installed; this is an unvalidated developer experiment");
            }
            catch (Exception ex)
            {
                Diagnostics.Write("Activation failed: " + ex);
                // A failed guard must never install a partial method replacement.
            }
        }

        private static bool Sha256Equals(byte[] data, string reference)
        {
            using (SHA256 sha = SHA256.Create())
            {
                byte[] hash = sha.ComputeHash(data);
                if (hash.Length != 32 || reference.Length != 64) return false;
                for (int i = 0; i < hash.Length; i++)
                {
                    if (hash[i].ToString("x2") != reference.Substring(2 * i, 2))
                        return false;
                }
                return true;
            }
        }
    }

    internal static class Diagnostics
    {
        public static void Write(string message)
        {
            try
            {
                string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                    @"Electronic Arts\The Sims 3\Apex Radiance");
                if (!Directory.Exists(folder)) Directory.CreateDirectory(folder);
                File.AppendAllText(Path.Combine(folder, "cas-hair-experiment.log"),
                    DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " " + message + Environment.NewLine);
            }
            catch (Exception) { }
        }
    }

    public sealed class HairGridReplacement
    {
        private static int sGeneration;

        // The method is private in UI.dll, and MonoPatcher swaps the method body before its first JIT.
        // On invocation, 'this' is a CASHair instance (MonoPatcher's documented instance-method convention).
        [ReplaceMethod(typeof(CASHair), "PopulateTypesGrid", new Type[] { typeof(bool) })]
        private void PopulateTypesGrid(bool unusedOriginalFlag)
        {
            CASHair ui = (CASHair)(this as object);
            if (ui == null) return;
            // Increment even if the old task has not yet yielded: it must not touch the next category.
            int ticket = Interlocked.Increment(ref sGeneration);

            try
            {
                HairJob job = new HairJob(ui, ticket);
                job.Begin();
            }
            catch (Exception ex)
            {
                Diagnostics.Write("Could not create hair job: " + ex);
                // This experimental replacement cannot call the old method once patched.
                // Do not expose this prototype to an ordinary game installation.
            }
        }

        private sealed class HairJob
        {
            private const long BudgetMilliseconds = 2;
            private const int MaxEntriesBetweenYields = 3;

            private readonly CASHair ui;
            private readonly int generation;
            private readonly CASHair.HairType category;
            private readonly ICASModel model;
            private readonly CASAgeGenderFlags age;
            private readonly CASAgeGenderFlags gender;
            private readonly CASAgeGenderFlags species;
            private readonly OutfitCategories outfit;
            private readonly ItemGrid targetGrid;
            private readonly ResourceKey layout;
            private readonly CASPart originallyWorn;
            private readonly Color[] blankColors = {
                new Color(0), new Color(0), new Color(0), new Color(0)
            };
            private readonly List<object> storeParts = new List<object>();
            private readonly List<CASPart> casParts = new List<CASPart>();

            private int storePosition;
            private int partPosition;
            private int yields;
            private int presetPosition;
            private bool processingHatPresets;
            private CASPart activePart;
            private bool activeWardrobe;
            private bool selected;
            private bool filterable;
            private string wornPresetXml = "";
            private string wornPresetWithDefaultColors = "";

            public HairJob(CASHair target, int ticket)
            {
                ui = target;
                generation = ticket;
                category = ui.mHairType;
                model = Responder.Instance.CASModel;
                age = model.Age;
                gender = model.Gender;
                species = model.Species;
                outfit = model.OutfitCategory;
                targetGrid = ui.mHairTypesGrid;
                layout = ResourceKey.CreateUILayoutKey("GenericCasItem", 0);
                originallyWorn = ui.GetWornPart();

                // Snapshot the catalog. Never keep a live list enumerator across Simulator.Sleep.
                foreach (CASPart part in ui.mPartsList)
                    casParts.Add(part);
            }

            private bool Current()
            {
                if (Interlocked.CompareExchange(ref sGeneration, 0, 0) != generation ||
                    CASHair.gSingleton != ui || ui.mHairType != category)
                    return false;
                if (Responder.Instance == null || Responder.Instance.CASModel != model)
                    return false;
                if (ui.mHairTypesGrid != targetGrid)
                    return false;
                return model.Age == age && model.Gender == gender &&
                       model.Species == species && model.OutfitCategory == outfit;
            }

            public void Begin()
            {
                if (!Current()) return;
                targetGrid.Clear();

                if (category == CASHair.HairType.Hat)
                {
                    ui.mHatsShareButton.Enabled = false;
                    ui.mHatsDeleteButton.Enabled = false;
                    ui.mDesignButton.Enabled = CASHair.PartIsHat(originallyWorn);
                }

                // Store items are the first logical grid entries in the stock browser.
                List<object> featured = Responder.Instance.StoreUI.GetCASFeaturedStoreItems(
                    BodyTypes.Hair, model.OutfitCategory,
                    (model.Age | model.Species) | model.Gender,
                    category == CASHair.HairType.Hat);
                ui.mContentTypeFilter.FilterObjects(featured, out filterable);
                storeParts.AddRange(featured);

                // Runs as a simulator task; Never yield directly from a UI click callback.
                Simulator.AddObject(new Sims3.UI.OneShotFunctionTask(
                    new Sims3.UI.Function(Process)));
            }

            private void Process()
            {
                try
                {
                    Stopwatch watch = Stopwatch.StartNew();
                    int processed = 0;

                    // Store rows first, then parts. Append-only ensures stable indices of already-rendered rows.
                    while (Current() && storePosition < storeParts.Count)
                    {
                        AddStoreRow(storeParts[storePosition++]);
                        if (++processed >= MaxEntriesBetweenYields || watch.ElapsedMilliseconds >= BudgetMilliseconds)
                        {
                            ++yields;
                            Simulator.Sleep(0); // cooperative yield on a simulator task, not a UI callback
                            if (!Current()) return;
                            watch.Reset();
                            watch.Start();
                            processed = 0;
                        }
                    }

                    while (Current() && (partPosition < casParts.Count || processingHatPresets))
                    {
                        if (!processingHatPresets)
                        {
                            activePart = casParts[partPosition++];
                            AddDefaultPart(activePart);

                            if (category == CASHair.HairType.Hat &&
                                !UIUtils.IsContentTypeDisabled(UIUtils.GetCustomContentType(activePart.Key)))
                            {
                                presetPosition = 0;
                                processingHatPresets = true;
                            }
                        }
                        else
                        {
                            if (!AddNextHatPreset())
                                processingHatPresets = false;
                        }

                        if (++processed >= MaxEntriesBetweenYields || watch.ElapsedMilliseconds >= BudgetMilliseconds)
                        {
                            ++yields;
                            Simulator.Sleep(0);
                            if (!Current()) return;
                            watch.Reset();
                            watch.Start();
                            processed = 0;
                        }
                    }

                    if (Current()) Finish();
                    else Diagnostics.Write("Discarded stale hair job " + generation);
                }
                catch (Exception ex)
                {
                    Diagnostics.Write("Hair job failed after " + partPosition + " parts: " + ex);
                }
            }

            private void AddStoreRow(object source)
            {
                IFeaturedStoreItem item = source as IFeaturedStoreItem;
                if (item == null) return;
                bool isHat = (item.CategoryFlags & 0x400000) != 0;
                if (isHat != (category == CASHair.HairType.Hat)) return;

                WindowBase window = UIManager.LoadLayout(layout).GetWindowByExportID(1);
                if (window == null) return;
                window.Tag = item;
                window.GetChildByID(0x23, true);
                Window icon = window.GetChildByID(0x20, true) as Window;
                if (icon != null)
                {
                    ImageDrawable drawable = icon.Drawable as ImageDrawable;
                    if (drawable != null)
                    {
                        drawable.Image = UIUtils.GetUIImageFromThumbnailKey(item.ThumbKey);
                        icon.Invalidate();
                    }
                }
                Window priceArea = window.GetChildByID(0x300, true) as Window;
                if (priceArea != null)
                {
                    priceArea.Tag = item;
                    priceArea.CreateTooltipCallbackFunction = ui.StoreItemCreateTooltip;
                    priceArea.Visible = true;
                }
                Window sale = window.GetChildByID(0x303, true) as Window;
                if (sale != null) sale.Visible = item.IsSale;

                Button buy = window.GetChildByID(0x301, true) as Button;
                if (buy != null)
                {
                    buy.Caption = item.PriceString;
                    buy.Tag = window;
                    buy.Click += ui.OnBuyButtonClick;
                    buy.FocusAcquired += ui.OnBuyButtonFocusAcquired;
                    buy.FocusLost += ui.OnBuyButtonFocusLost;
                }
                targetGrid.AddItem(new ItemGridCellItem(window, item));
            }

            private void AddDefaultPart(CASPart part)
            {
                if (part == null) return;
                if (UIUtils.IsContentTypeDisabled(UIUtils.GetCustomContentType(part.Key)))
                    return;

                // ObjectDesigner is a global native state machine: don't retain its state across yields.
                ObjectDesigner.SetCASPart(part.Key);
                activeWardrobe = model.ActiveWardrobeContains(part);
                uint selectedPresetIndex = ObjectDesigner.GetDesignPresetIndexFromId(ObjectDesigner.DefaultPresetId);
                string xml = ObjectDesigner.GetDesignPreset(selectedPresetIndex);
                if (String.IsNullOrEmpty(xml))
                {
                    ResourceKey fallback = new ResourceKey(part.Key.InstanceId, 0x0333406C, part.Key.GroupId);
                    xml = Simulator.LoadXMLString(fallback);
                }
                CASPartPreset preset = new CASPartPreset(part, xml);
                wornPresetXml = "";
                wornPresetWithDefaultColors = "";
                if (originallyWorn.Key == part.Key)
                {
                    wornPresetXml = model.GetDesignPreset(originallyWorn);
                    wornPresetWithDefaultColors = CASUtils.ReplaceHairColors(wornPresetXml, blankColors);
                }

                if (preset.Valid && (category == CASHair.HairType.Hair ||
                                     ObjectDesigner.DefaultPresetId == UInt32.MaxValue))
                {
                    bool added = ui.AddHairTypeGridItem(
                        targetGrid, layout, preset, activeWardrobe, ref filterable);
                    if (added && originallyWorn.Key == part.Key &&
                        (category == CASHair.HairType.Hair ||
                         CASUtils.DesignPresetCompare(wornPresetXml, xml)))
                    {
                        targetGrid.SelectedItem = targetGrid.Count - 1;
                        selected = true;
                    }
                }
            }

            // One hat preset per step; no inner tight loop that blocks long frames.
            private bool AddNextHatPreset()
            {
                if (activePart == null) return false;
                ObjectDesigner.SetCASPart(activePart.Key);
                uint count = CASUtils.PartDataNumPresets(activePart.Key);
                if (presetPosition >= count) return false;

                uint index = (uint)presetPosition++;
                uint id = CASUtils.PartDataGetPresetId(activePart.Key, index);
                string xml = CASUtils.PartDataGetPreset(activePart.Key, index);
                CASPartPreset preset = new CASPartPreset(activePart, id, xml);
                if (!preset.Valid) return true;
                bool added = ui.AddHairTypeGridItem(
                    targetGrid, layout, preset, activeWardrobe, ref filterable);

                if (originallyWorn.Key == activePart.Key &&
                    CASUtils.DesignPresetCompare(wornPresetWithDefaultColors,
                                                CASUtils.ReplaceHairColors(xml, blankColors)))
                {
                    ui.mSavedPresetId = preset.mPresetId;
                    selected = true;
                    if (added)
                    {
                        targetGrid.SelectedItem = targetGrid.Count - 1;
                        if (ObjectDesigner.IsUserDesignPreset(index))
                        {
                            ui.mHatsShareButton.Enabled = true;
                            ui.mHatsDeleteButton.Enabled = true;
                        }
                    }
                }
                return true;
            }

            private void Finish()
            {
                targetGrid.Tag = filterable;
                if (ui.mHairStylesGrid.Tag == null) ui.mHairStylesGrid.Tag = false;
                ui.mSortButton.Tag = ((bool)targetGrid.Tag) || ((bool)ui.mHairStylesGrid.Tag);

                if (selected)
                    ui.mSaveButton.Enabled = false;
                else if (category == CASHair.HairType.Hat &&
                         CASHair.PartIsHat(originallyWorn))
                {
                    WindowBase placeholder = UIManager.LoadLayout(layout).GetWindowByExportID(1);
                    if (placeholder != null)
                    {
                        Window first = placeholder.GetChildByID(0x20, true) as Window;
                        Window second = placeholder.GetChildByID(0x24, true) as Window;
                        if (first != null) first.Visible = false;
                        if (second != null) second.Visible = true;
                        targetGrid.AddTempItem(new ItemGridCellItem(placeholder, null));
                    }
                    ui.mSaveButton.Enabled = true;
                }

                ui.mUndoOnDelete = false;
                ui.mContentTypeFilter.UpdateFilterButtonState();
                // Original callers may attempt this immediately after PopulateTypesGrid returns.
                // Repeat it after the last async row to restore the final displayed selection.
                if (category == CASHair.HairType.Hair)
                    ui.ReselectCurrentHairPresetItem();
                Diagnostics.Write("Hair job completed category=" + category +
                    " parts=" + partPosition + " store=" + storePosition + " yields=" + yields);
            }
        }
    }
}

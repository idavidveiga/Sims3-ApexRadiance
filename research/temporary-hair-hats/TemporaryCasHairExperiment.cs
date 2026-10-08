// RETIRED RESEARCH PROTOTYPE. Hair/Hats replacement was activated in a
// user test but prevented hairs and hats from appearing in Create a Sim.
// Never put the discarded full-method replacement in ApexRadiance.asi,
// an installable package, or any public release. See Git history for
// the experiment and docs/research/temporary-monopatcher-hair-hats.md.
// This safe diagnostic no longer changes any game methods.
using System;
using MonoPatcherLib;
using Sims3.SimIFace;
using Sims3.UI;

namespace ApexHairTemporaryResearch
{
    [Plugin(false)]
    public sealed class TemporaryCasHairExperiment
    {
        public TemporaryCasHairExperiment()
        {
            World.sOnStartupAppEventHandler += RegisterStatusCommand;
        }

        private static void RegisterStatusCommand(object sender, EventArgs e)
        {
            CommandSystem.RegisterCommand("apexhair_status",
                "Show temporary Apex Hair/Hats research status",
                (object[] args) =>
                {
                    SimpleMessageDialog.Show("Apex Hair Research",
                        "DISABLED: unsafe Hair/Hats method replacement withdrawn. " +
                        "Please remove ApexHairTemporaryResearch.package and " +
                        "restart the game. No game method is patched by this build.");
                    return 1;
                });
        }
    }
}

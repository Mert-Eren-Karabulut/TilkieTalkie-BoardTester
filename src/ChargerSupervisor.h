#pragma once

// Charger supervision while USB is present, in every phase (monitor and full run): charge
// current tiers, temperature window, PD input budget, full-charge hold, charge kicks.
// Called from loop() every pass.
void superviseChargerOnUsb();

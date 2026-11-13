// RecoveryRestart: ask the loader for Recovery on the next start and restart
// now. a separate tiny header so the settings page (kernel and ring-3) can
// call it: the ring-3 build sends the "boot.recovery" action instead. (satoru)
#pragma once
void RecoveryRestart();
// end (satoru)

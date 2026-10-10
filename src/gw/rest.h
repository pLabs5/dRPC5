#pragma once

/* Rest (standby/suspend) detection. The console publishes the current
   SystemStateMgr state in a named kernel event flag, low 16 bits; this module
   opens it and watches the transitions into and out of rest mode so the
   gateway can park a live session instead of letting it freeze mid-write.

   While the console is actually in standby the whole process is frozen, so the
   signal races between the monitor thread (which keeps running through the
   suspend handshake) and the gateway thread, which parks on rest_active() and
   reconnects fresh on wake. */

int rest_start(void);
void rest_stop(void);
/* Non-zero while the console is in (or heading into) rest mode. */
int rest_active(void);
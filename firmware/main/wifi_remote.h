#pragma once

/**
 * Remote Wi-Fi change (milestone 4): the staged network is committed when the
 * ThingsBoard link connects over it, which proves the new network works.
 */

/** tb_link connected hook: commits a staged Wi-Fi network (stg_type == 1) and cancels the rollback timers. */
void wifi_remote_on_tb_connected(void);

/** Deferred action after the RPC reply is acked: restart into the staged network. */
void wifi_remote_restart_for_stage(void);

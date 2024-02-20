/**
 * @file vlc_control9P_plugin.c
 * @brief VLC control module for the 9P protocol
 */

#define MODULE_STRING "control9P"

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#define IXP_NO_P9_
#define IXP_P9_STRUCTS
#include <ixp.h>
// #include <ixp_srvutil.h>

#include <stdlib.h>
/* VLC core API headers */
#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_interface.h>

static int Run(vlc_object_t *obj);

/* Internal state for an instance of the module */
struct intf_sys_t
{
};

/**
 * Init the control module
 */
static int
Open(vlc_object_t *obj)
{
    intf_thread_t *p_intf = (intf_thread_t *)obj;
    msg_Info(p_intf, "using the 9P control interface");

    /* Allocate internal state */
    intf_sys_t *sys = malloc(sizeof (*sys));
    if (unlikely(sys == NULL))
        return VLC_ENOMEM;
    p_intf->p_sys = sys;

    /* Set up p_intf */
    // p_intf->pf_run = Run;
    return VLC_SUCCESS;

error:
    free(sys);
    return VLC_EGENERIC;    
}

/**
 * Terminate the control module
 */
static void
Close(vlc_object_t *obj)
{
    intf_thread_t *p_intf = (intf_thread_t *)obj;
    intf_sys_t *sys = p_intf->p_sys;

    /* Free internal state */
    free(sys);
}

// static int
// Run(vlc_object_t *obj)
// {
    // intf_thread_t *p_intf = (intf_thread_t *)obj;
// }

static void AutoRun(libvlc_int_t *libvlc)
{
    // intf_Create(libvlc, MODULE_STRING);
}

/* Module descriptor */
vlc_module_begin()
    set_shortname("9P control")
    set_description("Controling the player using the 9P protocol")
    set_capability("interface", 60)
    set_callbacks(Open, Close)
    set_subcategory(SUBCAT_INTERFACE_CONTROL)
vlc_module_end()
// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * acerhdf - A driver which monitors the temperature
 *           of the aspire one netbook, turns on/off the fan
 *           as soon as the upper/lower threshold is reached.
 *
 * (C) 2009 - Peter Feuerer     peter (a) piie.net
 *                              http://piie.net
 *     2009 Borislav Petkov	bp (a) alien8.de
 *     **************** Acer Predator Helios 500 mod ****************
 *     2019 ArSi                arsi(a) arsi.sk
 * 
 *
 * Inspired by and many thanks to:
 *  o acerfand   - Rachel Greenham
 *  o acer_ec.pl - Michael Kurz     michi.kurz (at) googlemail.com
 *               - Petr Tomasek     tomasek (#) etf,cuni,cz
 *               - Carlos Corbacho  cathectic (at) gmail.com
 *  o lkml       - Matthew Garrett
 *               - Borislav Petkov
 *               - Andreas Mohr
 * 
 * 
 * 
 * **************** Acer Predator Helios 500 mod ****************
 * Mod to allow more aggressive CPU fan speed control settings
 * Verion 0.2 Beta
 * 
 * The driver is tested on Predator PH517-51 bios V1.06
 * If you have another version of predator please first check if the EC registers also apply to your Predator.
 * I searched the registers according to the following instructions:
 * https://github.com/hirschmann/nbfc/wiki/Probe-the-EC%27s-registers
 * 
 * Then add your predator to the variable: bios_settings 
 * {"Acer", "Predator PH517-51", "V1.06", 0x4f, 0x58, {0x14, 0x04}, 1},
 *                   ^              ^       ^    ^    ^^^^^^^^^^^^^^^^^
 *                 Model           Bios    Fan  Temp   Unussed
 * 
 * Please do not forget to do a PR to make life easier for others
 * 
 * The current fancontrol table is hardcoded in function acerhdf_set_cur_state
 * It is quite aggressive, you can adjust it according to your requirements.
 * 
 * TODO:
 *      - code cleanup
 *      
 * Added Linux shutdown after reaching the CPU temperature of 89°C
 * 
 * 
 */

//*****************Predator settings *******************************************
//Calculate AVG temperatue from X samles, one sample 1s
#define TEMPERATURE_SAMPLES  10

//Minimal allowed Fan speed
#define MIN_FAN_SPEED 4

/*
 * According to the i7-8750H datasheet,
 * (https://ark.intel.com/content/www/us/en/ark/products/134906/intel-core-i7-8750h-processor-9m-cache-up-to-4-10-ghz.html) the
 * CPU's maximum temperature is 100°C
 *  So, assume 89°C is critical temperature.
 */
#define ACERHDF_TEMP_CRIT 89


static int fan_speed_debug = 0; //enable debug messages to dmesg
static unsigned int verbose = 0; //show orig driver debug messages
//******************************************************************************
#define pr_fmt(fmt) "acerhdf: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/dmi.h>
#include <linux/acpi.h>
#include <linux/thermal.h>
#include <linux/platform_device.h>

/*
 * The driver is started with "kernel mode off" by default. That means, the BIOS
 * is still in control of the fan. In this mode the driver allows to read the
 * temperature of the cpu and a userspace tool may take over control of the fan.
 * If the driver is switched to "kernel mode" (e.g. via module parameter) the
 * driver is in full control of the fan. If you want the module to be started in
 * kernel mode by default, define the following:
 */
#undef START_IN_KERNEL_MODE

#define DRV_VER "0.2 beta"


#define ACERHDF_FAN_OFF 0
#define ACERHDF_FAN_AUTO 1

/*
 * No matter what value the user puts into the fanon variable, turn on the fan
 * at 80 degree Celsius to prevent hardware damage
 */
#define ACERHDF_MAX_FANON 80000

/*
 * Maximum interval between two temperature checks is 15 seconds, as the die
 * can get hot really fast under heavy load (plus we shouldn't forget about
 * possible impact of _external_ aggressive sources such as heaters, sun etc.)
 */
#define ACERHDF_MAX_INTERVAL 15

#ifdef START_IN_KERNEL_MODE
static int kernelmode = 1;
#else
static int kernelmode = 1;
#endif

static unsigned int interval = 1;
static unsigned int fanon = 30;
static unsigned int fanoff = 53000;

static int samples[TEMPERATURE_SAMPLES];
static int current_sample = 0;



static unsigned int list_supported;
static unsigned int fanstate = ACERHDF_FAN_AUTO;
static char force_bios[16];
static char force_product[16];
static unsigned int prev_interval;
static struct thermal_zone_device *thz_dev;
static struct thermal_cooling_device *cl_dev;
static struct platform_device *acerhdf_dev;

module_param(kernelmode, uint, 0);
MODULE_PARM_DESC(kernelmode, "Kernel mode fan control on / off");
module_param(interval, uint, 0600);
MODULE_PARM_DESC(interval, "Polling interval of temperature check");
module_param(fanon, uint, 0600);
MODULE_PARM_DESC(fanon, "Turn the fan on above this temperature");
module_param(fanoff, uint, 0600);
MODULE_PARM_DESC(fanoff, "Turn the fan off below this temperature");
module_param(verbose, uint, 0600);
MODULE_PARM_DESC(verbose, "Enable verbose dmesg output");
module_param(list_supported, uint, 0600);
MODULE_PARM_DESC(list_supported, "List supported models and BIOS versions");
module_param_string(force_bios, force_bios, 16, 0);
MODULE_PARM_DESC(force_bios, "Pretend system has this known supported BIOS version");
module_param_string(force_product, force_product, 16, 0);
MODULE_PARM_DESC(force_product, "Pretend system is this known supported model");

/*
 * cmd_off: to switch the fan completely off and check if the fan is off
 *	cmd_auto: to set the BIOS in control of the fan. The BIOS regulates then
 *		the fan speed depending on the temperature
 */
struct fancmd {
    u8 cmd_off;
    u8 cmd_auto;
};

struct manualcmd {
    u8 mreg;
    u8 moff;
};

/* default register and command to disable fan in manual mode */
static const struct manualcmd mcmd = {
    .mreg = 0x94,
    .moff = 0xff,
};

/* BIOS settings - only used during probe */
struct bios_settings {
    const char *vendor;
    const char *product;
    const char *version;
    u8 fanreg;
    u8 tempreg;
    struct fancmd cmd;
    int mcmd_enable;
};

/* This could be a daughter struct in the above, but not worth the redirect */
struct ctrl_settings {
    u8 fanreg;
    u8 tempreg;
    struct fancmd cmd;
    int mcmd_enable;
};

static struct ctrl_settings ctrl_cfg __read_mostly;

/* Register addresses and values for different BIOS versions */
static const struct bios_settings bios_tbl[] __initconst = {
    /* Acer Predator PH517-51/Cayman_CFS, BIOS V1.06 05/03/2018   */
    {"Acer", "Predator PH517-51", "V1.06", 0x4f, 0x58,
        {0x14, 0x04}, 1},
    /* pewpew-terminator */
    {"", "", "", 0, 0,
        {0, 0}, 0}
};

/*
 * this struct is used to instruct thermal layer to use bang_bang instead of
 * default governor for acerhdf
 */
static struct thermal_zone_params acerhdf_zone_params = {
    .governor_name = "bang_bang",
};

static int acerhdf_get_temp(int *temp) {
    u8 read_temp;

    if (ec_read(ctrl_cfg.tempreg, &read_temp))
        return -EINVAL;

    *temp = read_temp;

    return 0;
}

static int acerhdf_get_fanstate(int *state) {
    u8 fan;

    if (ec_read(ctrl_cfg.fanreg, &fan))
        return -EINVAL;

    *state = (int) fan;

    return 0;
}

static void acerhdf_change_fanstate(int state) {
    fanstate = state;
    if (fan_speed_debug) {
        pr_notice("Fan speed: %i\n", state);
    }

    ec_write(ctrl_cfg.fanreg, (unsigned char) state);
}

static void acerhdf_check_param(struct thermal_zone_device *thermal) {
    if (fanon > ACERHDF_MAX_FANON) {
        pr_err("fanon temperature too high, set to %d\n",
                ACERHDF_MAX_FANON);
        fanon = ACERHDF_MAX_FANON;
    }

    if (kernelmode && prev_interval != interval) {
        if (interval > ACERHDF_MAX_INTERVAL) {
            pr_err("interval too high, set to %d\n",
                    ACERHDF_MAX_INTERVAL);
            interval = ACERHDF_MAX_INTERVAL;
        }
        if (verbose)
            pr_notice("interval changed to: %d\n", interval);
        thermal->polling_delay = interval * 1000;
        prev_interval = interval;
    }
}

/*
 * This is the thermal zone callback which does the delayed polling of the fan
 * state. We do check /sysfs-originating settings here in acerhdf_check_param()
 * as late as the polling interval is since we can't do that in the respective
 * accessors of the module parameters.
 */
static int acerhdf_get_ec_temp(struct thermal_zone_device *thermal, int *t) {
    int temp, err = 0;

    acerhdf_check_param(thermal);

    err = acerhdf_get_temp(&temp);
    if (err)
        return err;
    *t = temp;
    return 0;
}

static int acerhdf_bind(struct thermal_zone_device *thermal,
        struct thermal_cooling_device *cdev) {
    /* if the cooling device is the one from acerhdf bind it */
    if (cdev != cl_dev)
        return 0;

    if (thermal_zone_bind_cooling_device(thermal, 0, cdev,
            THERMAL_NO_LIMIT, THERMAL_NO_LIMIT,
            THERMAL_WEIGHT_DEFAULT)) {
        pr_err("error binding cooling dev\n");
        return -EINVAL;
    }
    return 0;
}

static int acerhdf_unbind(struct thermal_zone_device *thermal,
        struct thermal_cooling_device *cdev) {
    if (cdev != cl_dev)
        return 0;

    if (thermal_zone_unbind_cooling_device(thermal, 0, cdev)) {
        pr_err("error unbinding cooling dev\n");
        return -EINVAL;
    }
    return 0;
}

static inline void acerhdf_revert_to_bios_mode(void) {
    acerhdf_change_fanstate(5);
    kernelmode = 0;
    if (thz_dev)
        thz_dev->polling_delay = 0;
    pr_notice("kernel mode fan control OFF\n");
}

static inline void acerhdf_enable_kernelmode(void) {
    kernelmode = 1;

    thz_dev->polling_delay = interval * 1000;
    thermal_zone_device_update(thz_dev, THERMAL_EVENT_UNSPECIFIED);
    pr_notice("kernel mode fan control ON\n");
}

static int acerhdf_get_mode(struct thermal_zone_device *thermal,
        enum thermal_device_mode *mode) {
    if (verbose)
        pr_notice("kernel mode fan control %d\n", kernelmode);

    *mode = (kernelmode) ? THERMAL_DEVICE_ENABLED
            : THERMAL_DEVICE_DISABLED;

    return 0;
}

/*
 * set operation mode;
 * enabled: the thermal layer of the kernel takes care about
 *          the temperature and the fan.
 * disabled: the BIOS takes control of the fan.
 */
static int acerhdf_set_mode(struct thermal_zone_device *thermal,
        enum thermal_device_mode mode) {
    if (mode == THERMAL_DEVICE_DISABLED && kernelmode)
        acerhdf_revert_to_bios_mode();
    else if (mode == THERMAL_DEVICE_ENABLED && !kernelmode)
        acerhdf_enable_kernelmode();

    return 0;
}

static int acerhdf_get_trip_type(struct thermal_zone_device *thermal, int trip,
        enum thermal_trip_type *type) {
    if (trip == 0)
        *type = THERMAL_TRIP_ACTIVE;
    else if (trip == 1)
        *type = THERMAL_TRIP_CRITICAL;
    else
        return -EINVAL;

    return 0;
}

static int acerhdf_get_trip_hyst(struct thermal_zone_device *thermal, int trip,
        int *temp) {
    if (trip != 0)
        return -EINVAL;

    *temp = fanon - fanoff;

    return 0;
}

static int acerhdf_get_trip_temp(struct thermal_zone_device *thermal, int trip,
        int *temp) {
    if (trip == 0)
        *temp = fanon;
    else if (trip == 1)
        *temp = ACERHDF_TEMP_CRIT;
    else
        return -EINVAL;

    return 0;
}

static int acerhdf_get_crit_temp(struct thermal_zone_device *thermal,
        int *temperature) {
    *temperature = ACERHDF_TEMP_CRIT;
    return 0;
}

/* bind callback functions to thermalzone */
static struct thermal_zone_device_ops acerhdf_dev_ops = {
    .bind = acerhdf_bind,
    .unbind = acerhdf_unbind,
    .get_temp = acerhdf_get_ec_temp,
    .get_mode = acerhdf_get_mode,
    .set_mode = acerhdf_set_mode,
    .get_trip_type = acerhdf_get_trip_type,
    .get_trip_hyst = acerhdf_get_trip_hyst,
    .get_trip_temp = acerhdf_get_trip_temp,
    .get_crit_temp = acerhdf_get_crit_temp,
};

/*
 * cooling device callback functions
 * get maximal fan cooling state
 */
static int acerhdf_get_max_state(struct thermal_cooling_device *cdev,
        unsigned long *state) {
    *state = 11;

    return 0;
}

static int acerhdf_get_cur_state(struct thermal_cooling_device *cdev,
        unsigned long *state) {
    int err = 0, tmp;

    err = acerhdf_get_fanstate(&tmp);
    if (err)
        return err;

    *state = (unsigned long) tmp;
    return 0;
}

/* change current fan state - is overwritten when running in kernel mode */
static int acerhdf_set_cur_state(struct thermal_cooling_device *cdev,
        unsigned long state) {
    int cur_temp, cur_state, err,i = 0;
    if (!kernelmode)
        return 0;

    err = acerhdf_get_temp(&samples[current_sample]);
    current_sample++;
    if (current_sample > TEMPERATURE_SAMPLES - 1) {
        current_sample = 0;
    }
    if (err) {
        pr_err("error reading temperature, hand off control to BIOS\n");
        goto err_out;
    }
    cur_temp = 0;
    for (i = 0; i < TEMPERATURE_SAMPLES; i++) {
        cur_temp+= samples[i];
    }
    cur_temp = cur_temp/TEMPERATURE_SAMPLES;


    err = acerhdf_get_fanstate(&cur_state);
    if (err) {
        pr_err("error reading fan state, hand off control to BIOS\n");
        goto err_out;
    }
    if (fan_speed_debug) {
        pr_notice("AVG Temperature: %i\n", cur_temp);
    }

    if (cur_temp < 40) {
        state = 3;
    } else if (cur_temp < 45) {
        state = 4;
    } else if (cur_temp < 48) {
        state = 5;
    } else if (cur_temp < 50) {
        state = 6;
    } else if (cur_temp < 55) {
        state = 7;
    } else if (cur_temp < 60) {
        state = 8;
    } else if (cur_temp < 65) {
        state = 9;
    } else if (cur_temp < 70) {
        state = 10;
    } else {
        state = 11;
    }
    if(state < MIN_FAN_SPEED){
        state = MIN_FAN_SPEED;
    }
    acerhdf_change_fanstate((int) state);
    return 0;

err_out:
    acerhdf_revert_to_bios_mode();
    return -EINVAL;
}

/* bind fan callbacks to fan device */
static const struct thermal_cooling_device_ops acerhdf_cooling_ops = {
    .get_max_state = acerhdf_get_max_state,
    .get_cur_state = acerhdf_get_cur_state,
    .set_cur_state = acerhdf_set_cur_state,
};

/* suspend / resume functionality */
static int acerhdf_suspend(struct device *dev) {
    if (kernelmode)
        acerhdf_change_fanstate(5);

    if (verbose)
        pr_notice("going suspend\n");

    return 0;
}

static int acerhdf_probe(struct platform_device *device) {
    return 0;
}

static int acerhdf_remove(struct platform_device *device) {
    return 0;
}

static const struct dev_pm_ops acerhdf_pm_ops = {
    .suspend = acerhdf_suspend,
    .freeze = acerhdf_suspend,
};

static struct platform_driver acerhdf_driver = {
    .driver =
    {
        .name = "acerhdf",
        .pm = &acerhdf_pm_ops,
    },
    .probe = acerhdf_probe,
    .remove = acerhdf_remove,
};

/* checks if str begins with start */
static int str_starts_with(const char *str, const char *start) {
    unsigned long str_len = 0, start_len = 0;

    str_len = strlen(str);
    start_len = strlen(start);

    if (str_len >= start_len &&
            !strncmp(str, start, start_len))
        return 1;

    return 0;
}

/* check hardware */
static int __init acerhdf_check_hardware(void) {
    char const *vendor, *version, *product;
    const struct bios_settings *bt = NULL;
    int found = 0;

    /* get BIOS data */
    vendor = dmi_get_system_info(DMI_SYS_VENDOR);
    version = dmi_get_system_info(DMI_BIOS_VERSION);
    product = dmi_get_system_info(DMI_PRODUCT_NAME);

    if (!vendor || !version || !product) {
        pr_err("error getting hardware information\n");
        return -EINVAL;
    }

    pr_info("Acer Predator Helios 500 Fan driver, v.%s\n", DRV_VER);

    if (list_supported) {
        pr_info("List of supported Manufacturer/Model/BIOS:\n");
        pr_info("---------------------------------------------------\n");
        for (bt = bios_tbl; bt->vendor[0]; bt++) {
            pr_info("%-13s | %-17s | %-10s\n", bt->vendor,
                    bt->product, bt->version);
        }
        pr_info("---------------------------------------------------\n");
        return -ECANCELED;
    }

    if (force_bios[0]) {
        version = force_bios;
        pr_info("forcing BIOS version: %s\n", version);
        kernelmode = 0;
    }

    if (force_product[0]) {
        product = force_product;
        pr_info("forcing BIOS product: %s\n", product);
        kernelmode = 0;
    }

    if (verbose)
        pr_info("BIOS info: %s %s, product: %s\n",
            vendor, version, product);

    memset(samples, 0, TEMPERATURE_SAMPLES * sizeof (int));

    /* search BIOS version and vendor in BIOS settings table */
    for (bt = bios_tbl; bt->vendor[0]; bt++) {
        /*
         * check if actual hardware BIOS vendor, product and version
         * IDs start with the strings of BIOS table entry
         */
        if (str_starts_with(vendor, bt->vendor) &&
                str_starts_with(product, bt->product) &&
                str_starts_with(version, bt->version)) {
            found = 1;
            break;
        }
    }

    if (!found) {
        pr_err("unknown (unsupported) BIOS version %s/%s/%s, please report, aborting!\n",
                vendor, product, version);
        return -EINVAL;
    }

    /* Copy control settings from BIOS table before we free it. */
    ctrl_cfg.fanreg = bt->fanreg;
    ctrl_cfg.tempreg = bt->tempreg;
    memcpy(&ctrl_cfg.cmd, &bt->cmd, sizeof (struct fancmd));
    ctrl_cfg.mcmd_enable = bt->mcmd_enable;

    /*
     * if started with kernel mode off, prevent the kernel from switching
     * off the fan
     */
    if (!kernelmode) {
        pr_notice("Fan control off, to enable do:\n");
        pr_notice("echo -n \"enabled\" > /sys/class/thermal/thermal_zoneN/mode # N=0,1,2...\n");
    }

    return 0;
}

static int __init acerhdf_register_platform(void) {
    int err = 0;

    err = platform_driver_register(&acerhdf_driver);
    if (err)
        return err;

    acerhdf_dev = platform_device_alloc("acerhdf", -1);
    if (!acerhdf_dev) {
        err = -ENOMEM;
        goto err_device_alloc;
    }
    err = platform_device_add(acerhdf_dev);
    if (err)
        goto err_device_add;

    return 0;

err_device_add:
    platform_device_put(acerhdf_dev);
err_device_alloc:
    platform_driver_unregister(&acerhdf_driver);
    return err;
}

static void acerhdf_unregister_platform(void) {
    platform_device_unregister(acerhdf_dev);
    platform_driver_unregister(&acerhdf_driver);
}

static int __init acerhdf_register_thermal(void) {
    cl_dev = thermal_cooling_device_register("acerhdf-fan", NULL,
            &acerhdf_cooling_ops);

    if (IS_ERR(cl_dev))
        return -EINVAL;

    thz_dev = thermal_zone_device_register("acerhdf", 2, 0, NULL,
            &acerhdf_dev_ops,
            &acerhdf_zone_params, 0,
            (kernelmode) ? interval * 1000 : 0);
    if (IS_ERR(thz_dev))
        return -EINVAL;

    if (strcmp(thz_dev->governor->name,
            acerhdf_zone_params.governor_name)) {
        pr_err("Didn't get thermal governor %s, perhaps not compiled into thermal subsystem.\n",
                acerhdf_zone_params.governor_name);
        return -EINVAL;
    }

    return 0;
}

static void acerhdf_unregister_thermal(void) {
    if (cl_dev) {
        thermal_cooling_device_unregister(cl_dev);
        cl_dev = NULL;
    }

    if (thz_dev) {
        thermal_zone_device_unregister(thz_dev);
        thz_dev = NULL;
    }
}

static int __init acerhdf_init(void) {
    int err = 0;

    err = acerhdf_check_hardware();
    if (err)
        goto out_err;

    err = acerhdf_register_platform();
    if (err)
        goto out_err;

    err = acerhdf_register_thermal();
    if (err)
        goto err_unreg;

    return 0;

err_unreg:
    acerhdf_unregister_thermal();
    acerhdf_unregister_platform();

out_err:
    return err;
}

static void __exit acerhdf_exit(void) {
    acerhdf_change_fanstate(5);
    acerhdf_unregister_thermal();
    acerhdf_unregister_platform();
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Peter Feuerer");
MODULE_DESCRIPTION("Aspire One temperature and fan driver");
MODULE_ALIAS("dmi:*:*Acer*:pnAOA*:");
MODULE_ALIAS("dmi:*:*Acer*:pnAO751h*:");
MODULE_ALIAS("dmi:*:*Acer*:pnAspire*1410*:");
MODULE_ALIAS("dmi:*:*Acer*:pnAspire*1810*:");
MODULE_ALIAS("dmi:*:*Acer*:pnAspire*5755G:");
MODULE_ALIAS("dmi:*:*Acer*:pnAspire*1825PTZ:");
MODULE_ALIAS("dmi:*:*Acer*:pnAO521*:");
MODULE_ALIAS("dmi:*:*Acer*:pnAO531*:");
MODULE_ALIAS("dmi:*:*Acer*:pnAspire*5739G:");
MODULE_ALIAS("dmi:*:*Acer*:pnAspire*One*753:");
MODULE_ALIAS("dmi:*:*Acer*:pnAspire*5315:");
MODULE_ALIAS("dmi:*:*Acer*:TravelMate*7730G:");
MODULE_ALIAS("dmi:*:*Acer*:TM8573T:");
MODULE_ALIAS("dmi:*:*Gateway*:pnAOA*:");
MODULE_ALIAS("dmi:*:*Gateway*:pnLT31*:");
MODULE_ALIAS("dmi:*:*Packard*Bell*:pnAOA*:");
MODULE_ALIAS("dmi:*:*Packard*Bell*:pnDOA*:");
MODULE_ALIAS("dmi:*:*Packard*Bell*:pnDOTMU*:");
MODULE_ALIAS("dmi:*:*Packard*Bell*:pnENBFT*:");
MODULE_ALIAS("dmi:*:*Packard*Bell*:pnDOTMA*:");
MODULE_ALIAS("dmi:*:*Packard*Bell*:pnDOTVR46*:");
MODULE_ALIAS("dmi:*:*Acer*:pnExtensa 5420*:");

module_init(acerhdf_init);
module_exit(acerhdf_exit);

/* Copyright (c) 2016-2017 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/fb.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/wakelock.h>

#define BATT_DRV_NAME	"lge_battery"

#define pr_bm(reason, format, ...)					\
	do {								\
		if (debug_mask & (reason))				\
			pr_info("%s: %s: " format, BATT_DRV_NAME,	\
					__func__, ##__VA_ARGS__);	\
		else							\
			pr_debug("%s: %s: " format, BATT_DRV_NAME,	\
					__func__, ##__VA_ARGS__);	\
	} while (0)

#define NORM_VOLT			4400000
#define LIM_VOLT			4100000
#define PARALLEL_VOLT			4450000
#define SC_VOLT				4200000
#define CHG_CURRENT_MAX			3550000
#define SC_CURRENT			2400000
#define LCD_ON_CURRENT			1000000
#define WATCH_DELAY			30000

enum debug_mask_print {
	ASSERT = BIT(0),
	ERROR = BIT(1),
	INTERRUPT = BIT(2),
	REGISTER = BIT(3),
	MISC = BIT(4),
	VERBOSE = BIT(5),
};

enum bm_vote_reason {
	BM_REASON_DEFAULT,
	BM_REASON_LCD,
	BM_REASON_STEP,
	BM_REASON_THERM,
	BM_REASON_MAX,
};

enum bm_therm_states {
	BM_HEALTH_COLD,
	BM_HEALTH_COOL,
	BM_HEALTH_GOOD,
	BM_HEALTH_WARM,
	BM_HEALTH_HOT,
	BM_HEALTH_MAX,
};

struct battery_manager {
	struct device			*dev;
	struct power_supply		*batt_psy;
	struct power_supply		*usb_psy;
	struct power_supply		*pl_psy;
	struct notifier_block		ps_nb;
	struct notifier_block		fb_nb;
	struct work_struct		bm_batt_update;
	struct work_struct		bm_usb_update;
	struct work_struct		bm_fb_update;
	struct delayed_work		bm_watch;
	struct wake_lock		chg_wake_lock;
	struct mutex			work_lock;

	enum bm_therm_states		therm_stat;
	int		chg_present;
	int		chg_status;
	int		batt_temp;
	int		fb_state;
	int		bm_vote_fcc_reason;
	int		bm_vote_fcc_value;
	bool		sc_status;
};

struct bm_therm_table {
	int		min;
	int		max;
	int		cur;
};

static struct bm_therm_table therm_table[BM_HEALTH_MAX] = {
	{  INT_MIN,       20,        0},
	{        0,      220,   710000},
	{      200,      450,  -EINVAL},
	{      430,      550,   710000},
	{      530,  INT_MAX,        0},
};

static int bm_vote_fcc_table[BM_REASON_MAX] = {
	CHG_CURRENT_MAX,
	-EINVAL,
	-EINVAL,
	-EINVAL,
};

static int debug_mask = ERROR | INTERRUPT | MISC | VERBOSE;

static int bm_get_property(struct power_supply *psy,
			   enum power_supply_property prop, int *value)
{
	union power_supply_propval val = {0, };
	int rc = 0;

	if (!psy) {
		pr_bm(ERROR, "Couldn't get psy\n");
		return -EINVAL;
	}

	rc = power_supply_get_property(psy, prop, &val);
	if (rc < 0) {
		pr_bm(ERROR, "Couldn't get property %d, rc=%d\n", prop, rc);
		return rc;
	}

	*value = val.intval;
	return rc;
}

static int bm_set_property(struct power_supply *psy,
			   enum power_supply_property prop, int value)
{
	union power_supply_propval val = {0, };
	int rc = 0;

	if (!psy) {
		pr_bm(ERROR, "Couldn't get psy\n");
		return -EINVAL;
	}

	val.intval = value;
	rc = power_supply_set_property(psy, prop, &val);
	if (rc < 0)
		pr_bm(ERROR, "Couldn't set property %d, rc=%d\n", prop, rc);

	return rc;
}

static int bm_vote_fcc_update(struct battery_manager *bm)
{
	int fcc = INT_MAX;
	int reason = -EINVAL;
	int i, rc = 0;

	for (i = 0; i < BM_REASON_MAX; i++) {
		if (bm_vote_fcc_table[i] == -EINVAL)
			continue;
		if (fcc > bm_vote_fcc_table[i]) {
			fcc = bm_vote_fcc_table[i];
			reason = i;
		}
	}

	if (reason != bm->bm_vote_fcc_reason || fcc != bm->bm_vote_fcc_value) {
		if (fcc != bm->bm_vote_fcc_value) {
			rc = bm_set_property(
				bm->batt_psy,
				POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
				fcc);
			if (rc < 0) {
				pr_bm(ERROR,
				      "Couldn't set current, rc=%d\n", rc);
				return rc;
			}
		}
		bm->bm_vote_fcc_reason = reason;
		bm->bm_vote_fcc_value = fcc;
		pr_bm(MISC, "vote id[%d], set cur[%d]\n", reason, fcc);
	}

	return rc;
}

static int bm_vote_fcc(struct battery_manager *bm, int reason, int fcc)
{
	int rc = 0;

	bm_vote_fcc_table[reason] = fcc;
	rc = bm_vote_fcc_update(bm);
	if (rc < 0) {
		pr_bm(ERROR, "Couldn't vote id[%d] set cur[%d], rc=%d\n",
		      reason, fcc, rc);
		bm_vote_fcc_table[reason] = -EINVAL;
	}

	return rc;
}

static int bm_vote_fcc_get(struct battery_manager *bm)
{
	if (bm->bm_vote_fcc_reason == -EINVAL)
		return -EINVAL;

	return bm_vote_fcc_table[bm->bm_vote_fcc_reason];
}

void bm_check_therm_charging(struct battery_manager *bm)
{
	enum bm_therm_states stat = bm->therm_stat;
	int i, rc = 0;

	for (i = 0; i < BM_HEALTH_MAX; i++) {
		if (bm->batt_temp < therm_table[stat].min)
			stat--;
		else if (bm->batt_temp >= therm_table[stat].max)
			stat++;
		else
			break;
	}

	if (bm->therm_stat != stat) {
		pr_bm(MISC, "STATE[%d->%d] TEMP[%d] CUR[%d]\n", bm->therm_stat,
		      stat, bm->batt_temp, therm_table[stat].cur);
		if (bm->therm_stat <= BM_HEALTH_GOOD && stat >= BM_HEALTH_WARM) {
			rc = bm_set_property(bm->batt_psy,
					     POWER_SUPPLY_PROP_VOLTAGE_MAX,
					     LIM_VOLT);
		} else if (bm->therm_stat >= BM_HEALTH_WARM &&
			   stat <= BM_HEALTH_GOOD) {
			rc = bm_set_property(bm->batt_psy,
					     POWER_SUPPLY_PROP_VOLTAGE_MAX,
					     NORM_VOLT);
		}
		if (rc < 0) {
			pr_bm(ERROR, "Couldn't set float voltage rc=%d\n", rc);
			return;
		}
		rc = bm_vote_fcc(bm, BM_REASON_THERM, therm_table[stat].cur);
		if (rc < 0) {
			pr_bm(ERROR, "Couldn't set ibat current rc=%d\n", rc);
			return;
		}
		bm->therm_stat = stat;
	}
}

void bm_check_step_charging(struct battery_manager *bm, int volt)
{
	int rc = 0;

	if (!bm->chg_present) {
		if (bm->sc_status) {
			rc = bm_vote_fcc(bm, BM_REASON_STEP, -EINVAL);
			if (rc < 0) {
				pr_bm(ERROR,
				      "Couldn't set ibat curr rc=%d\n", rc);
				return;
			}
			bm->sc_status = false;
		}
		return;
	}

	if (!bm->sc_status && volt >= SC_VOLT) {
		rc = bm_vote_fcc(bm, BM_REASON_STEP, SC_CURRENT);
		if (rc < 0) {
			pr_bm(ERROR, "Couldn't set ibat curr rc=%d\n", rc);
			return;
		}
		bm->sc_status = true;
	}
}

static void bm_check_status(struct battery_manager *bm)
{
	if (!bm->chg_present ||
	    (bm->chg_present && bm->chg_status == POWER_SUPPLY_STATUS_FULL)) {
		if (wake_lock_active(&bm->chg_wake_lock)) {
			pr_bm(MISC, "chg_wake_unlocked\n");
			wake_unlock(&bm->chg_wake_lock);
		}
	} else if (bm->chg_present &&
		   bm->chg_status != POWER_SUPPLY_STATUS_FULL) {
		if (!wake_lock_active(&bm->chg_wake_lock)) {
			pr_bm(MISC, "chg_wake_locked\n");
			wake_lock(&bm->chg_wake_lock);
		}
	}
}

static void bm_watch_work(struct work_struct *work)
{
	struct battery_manager *bm = container_of(work,
						struct battery_manager,
						bm_watch.work);
	int rc, batt_volt, ibat = 0;

	mutex_lock(&bm->work_lock);

	rc = bm_get_property(bm->batt_psy,
			     POWER_SUPPLY_PROP_VOLTAGE_NOW, &batt_volt);
	if (rc < 0)
		pr_bm(ERROR, "Couldn't do bm_check_step_charging=%d\n", rc);
	else
		bm_check_step_charging(bm, batt_volt);

	rc = bm_get_property(bm->batt_psy,
			     POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX,
			     &ibat);

	pr_bm(VERBOSE, "PRESENT:%d, CHG_STAT:%d, THM_STAT:%d, " \
		       "BAT_TEMP:%d, BAT_VOLT:%d, VOTE_CUR:%d, SET_CUR:%d,\n",
	      bm->chg_present, bm->chg_status, bm->therm_stat,
	      bm->batt_temp, batt_volt, bm_vote_fcc_get(bm), ibat);

	mutex_unlock(&bm->work_lock);

	schedule_delayed_work(&bm->bm_watch,
			      msecs_to_jiffies(WATCH_DELAY));
}

static void bm_batt_update_work(struct work_struct *work)
{
	struct battery_manager *bm = container_of(work,
						struct battery_manager,
						bm_batt_update);
	int rc, batt_temp = bm->batt_temp;

	mutex_lock(&bm->work_lock);

	rc = bm_get_property(bm->batt_psy,
			     POWER_SUPPLY_PROP_STATUS, &bm->chg_status);
	if (rc < 0)
		goto error;

	bm_check_status(bm);

	rc = bm_get_property(bm->batt_psy,
			     POWER_SUPPLY_PROP_TEMP, &bm->batt_temp);
	if (rc < 0)
		goto error;

	if (bm->batt_temp != batt_temp)
		bm_check_therm_charging(bm);

error:
	mutex_unlock(&bm->work_lock);
}

static void bm_usb_update_work(struct work_struct *work)
{
	struct battery_manager *bm = container_of(work,
						struct battery_manager,
						bm_usb_update);
	int rc = 0;

	mutex_lock(&bm->work_lock);

	rc = bm_get_property(bm->usb_psy,
			     POWER_SUPPLY_PROP_PRESENT, &bm->chg_present);
	if (rc < 0)
		goto error;

	if (!bm->chg_present)
		bm_check_step_charging(bm, 0);

	bm_check_status(bm);

error:
	mutex_unlock(&bm->work_lock);
}

static int bm_ps_notifier_call(struct notifier_block *nb,
			       unsigned long ev, void *v)
{
	struct power_supply *psy = v;
	struct battery_manager *bm = container_of(nb,
						struct battery_manager,
						ps_nb);

	if (!strcmp(psy->desc->name, "battery")) {
		if (!bm->batt_psy)
			bm->batt_psy = psy;
		if (ev == PSY_EVENT_PROP_CHANGED && bm->batt_psy)
			schedule_work(&bm->bm_batt_update);
	}

	if (!strcmp(psy->desc->name, "usb")) {
		if (!bm->usb_psy)
			bm->usb_psy = psy;
		if (ev == PSY_EVENT_PROP_CHANGED && bm->usb_psy)
			schedule_work(&bm->bm_usb_update);
	}
	return NOTIFY_OK;
}

static int bm_ps_register_notifier(struct battery_manager *bm)
{
	int rc = 0;

	bm->ps_nb.notifier_call = bm_ps_notifier_call;
	rc = power_supply_reg_notifier(&bm->ps_nb);
	if (rc < 0)
		pr_bm(ERROR, "Couldn't register bm notifier = %d", rc);

	return rc;
}

static void bm_fb_update_work(struct work_struct *work)
{
	struct battery_manager *bm = container_of(work,
						struct battery_manager,
						bm_fb_update);
	mutex_lock(&bm->work_lock);

	if (!(bm->fb_state & BL_CORE_FBBLANK))
		bm_vote_fcc(bm, BM_REASON_LCD, LCD_ON_CURRENT);
	else
		bm_vote_fcc(bm, BM_REASON_LCD, -EINVAL);

	mutex_unlock(&bm->work_lock);
}

static int bm_fb_notifier_call(struct notifier_block *nb,
			       unsigned long ev, void *v)
{
	struct fb_event *evdata = v;
	struct battery_manager *bm = container_of(nb,
						struct battery_manager,
						fb_nb);
	int fb_blank = 0;

	if (ev != FB_EVENT_BLANK)
		return NOTIFY_OK;

	if (evdata && evdata->data) {
		fb_blank = *(int *)evdata->data;
		switch (fb_blank) {
		case FB_BLANK_UNBLANK:
			bm->fb_state &= ~BL_CORE_FBBLANK;
			break;
		case FB_BLANK_NORMAL:
		case FB_BLANK_VSYNC_SUSPEND:
		case FB_BLANK_HSYNC_SUSPEND:
		case FB_BLANK_POWERDOWN:
			bm->fb_state |= BL_CORE_FBBLANK;
			break;
		default:
			pr_bm(ERROR, "not used evdata=%d\n", fb_blank);
			break;
		}
		schedule_work(&bm->bm_fb_update);
	}
	return NOTIFY_OK;
}

static int bm_fb_register_notifier(struct battery_manager *bm)
{
	int rc = 0;

	bm->fb_nb.notifier_call = bm_fb_notifier_call;
	rc = fb_register_client(&bm->fb_nb);
	if (rc < 0)
		pr_bm(ERROR, "Couldn't register bm notifier = %d\n", rc);

	return rc;
}

static int bm_init(struct battery_manager *bm)
{
	int rc = 0;

	bm->fb_state = 0;
	bm->therm_stat = BM_HEALTH_GOOD;
	bm->bm_vote_fcc_reason = -EINVAL;
	bm->bm_vote_fcc_value = -EINVAL;

	bm->batt_psy = power_supply_get_by_name("battery");
	if (!bm->batt_psy) {
		pr_bm(ERROR, "Couldn't get batt_psy\n");
		return -ENODEV;
	}

	bm->usb_psy = power_supply_get_by_name("usb");
	if (!bm->usb_psy) {
		pr_bm(ERROR, "Couldn't get usb_psy\n");
		return -ENODEV;
	}

	bm->pl_psy = power_supply_get_by_name("parallel");
	if (!bm->pl_psy) {
		pr_bm(ERROR, "Couldn't get pl_psy\n");
		return -ENODEV;
	}

	rc = bm_get_property(bm->batt_psy,
			     POWER_SUPPLY_PROP_STATUS, &bm->chg_status);
	if (rc < 0)
		bm->chg_status = 0;

	rc = bm_get_property(bm->batt_psy,
			     POWER_SUPPLY_PROP_TEMP, &bm->batt_temp);
	if (rc < 0)
		bm->batt_temp = 25;

	rc = bm_get_property(bm->usb_psy,
			     POWER_SUPPLY_PROP_PRESENT, &bm->chg_present);
	if (rc < 0)
		bm->chg_present = 0;

	rc = bm_set_property(bm->pl_psy,
			     POWER_SUPPLY_PROP_VOLTAGE_MAX,
			     PARALLEL_VOLT);
	if (rc < 0)
		pr_bm(ERROR, "Couldn't set pl float voltage, rc=%d", rc);

	INIT_WORK(&bm->bm_fb_update, bm_fb_update_work);
	INIT_WORK(&bm->bm_batt_update, bm_batt_update_work);
	INIT_WORK(&bm->bm_usb_update, bm_usb_update_work);
	INIT_DELAYED_WORK(&bm->bm_watch, bm_watch_work);

	mutex_init(&bm->work_lock);
	wake_lock_init(&bm->chg_wake_lock,
		       WAKE_LOCK_SUSPEND, "bm_wake_lock");

	if (bm->chg_present)
		bm_check_status(bm);

	bm_check_therm_charging(bm);
	schedule_delayed_work(&bm->bm_watch,
			      msecs_to_jiffies(WATCH_DELAY));

	return 0;
}

static int lge_battery_probe(struct platform_device *pdev)
{
	struct battery_manager *bm;
	int rc = 0;

	bm = devm_kzalloc(&pdev->dev, sizeof(struct battery_manager),
			  GFP_KERNEL);
	if (!bm) {
		pr_bm(ERROR, "no memory\n");
		return -ENOMEM;
	}

	bm->dev = &pdev->dev;
	rc = bm_init(bm);
	if (rc < 0) {
		pr_bm(ERROR, "bm_init fail\n");
		return rc;
	}

	platform_set_drvdata(pdev, bm);

	rc = bm_ps_register_notifier(bm);
	if (rc < 0) {
		pr_bm(ERROR, "bm_power_register_notifier fail\n");
		goto error;
	}

	rc = bm_fb_register_notifier(bm);
	if (rc < 0) {
		pr_bm(ERROR, "bm_fb_register_notifier fail!\n");
		goto error;
	}

	pr_bm(VERBOSE, "Battery manager driver probe success!\n");
	return 0;

error:
	mutex_destroy(&bm->work_lock);
	platform_set_drvdata(pdev, NULL);
	return rc;
}

static int lge_battery_suspend(struct device *dev)
{
	struct battery_manager *bm = dev_get_drvdata(dev);

	if (!bm) {
		pr_bm(ERROR, "There is no battery manager\n");
		return -ENODEV;
	}
	cancel_delayed_work_sync(&bm->bm_watch);

	return 0;
}

static int lge_battery_resume(struct device *dev)
{
	struct battery_manager *bm = dev_get_drvdata(dev);

	if (!bm) {
		pr_bm(ERROR, "There is no battery manager\n");
		return -ENODEV;
	}
	schedule_delayed_work(&bm->bm_watch, 0);

	return 0;
}

static int lge_battery_remove(struct platform_device *pdev)
{
	struct battery_manager *bm = dev_get_drvdata(&pdev->dev);

	mutex_destroy(&bm->work_lock);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static const struct dev_pm_ops lge_battery_pm_ops = {
	.suspend	= lge_battery_suspend,
	.resume		= lge_battery_resume,
};

static struct platform_device lge_battery_pdev = {
	.name = BATT_DRV_NAME,
	.id = -1,
};

static struct platform_driver lge_battery_driver = {
	.probe = lge_battery_probe,
	.remove = lge_battery_remove,
	.driver = {
		.name = BATT_DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &lge_battery_pm_ops,
	},
};

static int __init lge_battery_init(void)
{
	int ret;

	ret = platform_device_register(&lge_battery_pdev);
	if (ret < 0) {
		pr_bm(ERROR, "device register fail\n");
		return ret;
	}

	ret = platform_driver_register(&lge_battery_driver);
	if (ret < 0) {
		pr_bm(ERROR, "driver register fail\n");
		platform_device_unregister(&lge_battery_pdev);
		return ret;
	}
	return 0;
}

static void __exit lge_battery_exit(void)
{
	platform_device_unregister(&lge_battery_pdev);
	platform_driver_unregister(&lge_battery_driver);
}

module_init(lge_battery_init);
module_exit(lge_battery_exit);
MODULE_LICENSE("GPL");

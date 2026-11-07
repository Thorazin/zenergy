// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2020 Advanced Micro Devices, Inc.
 * Edited by BouHaa (http://github.com/boukehaarsma23)
 */
#include <asm/cpu_device_id.h>

#include <linux/bits.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/processor.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include <linux/types.h>
#include <linux/version.h>

#define DRV_MODULE_DESCRIPTION	"AMD energy driver"
#define DRV_MODULE_VERSION	"1.0"

MODULE_VERSION(DRV_MODULE_VERSION);

#define DRVNAME			"zenergy"

#define ENERGY_PWR_UNIT_MSR	0xC0010299
#define ENERGY_CORE_MSR		0xC001029A
#define ENERGY_PKG_MSR		0xC001029B

#define zenergy_UNIT_MASK	0x01F00
#define zenergy_MASK		0xFFFFFFFF

struct sensor_accumulator {
	u64 energy_ctr;
	u64 prev_value;
	unsigned long cache_timeout;
};

struct zenergy_core {
	int package_id;
	int core_id;
};

struct zenergy_data {
	struct hwmon_channel_info energy_info;
	const struct hwmon_channel_info *info[2];
	struct hwmon_chip_info chip;
	struct task_struct *wrap_accumulate;
	/* Lock around the accumulator */
	struct mutex lock;
	/* An accumulator for each core and socket */
	struct sensor_accumulator *accums;
	unsigned int timeout_ms;
	/* Energy Status Units */
	int energy_units;
	int nr_cpus;
	int nr_socks;
	int *socket_ids;
	struct zenergy_core *cores;
	int core_id;
	char (*label)[10];
	bool do_not_accum;
};

static int zenergy_socket_cpu(struct zenergy_data *data, int socket)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (topology_physical_package_id(cpu) == data->socket_ids[socket])
			return cpu;
	}

	return -ENODEV;
}

static int zenergy_core_cpu(struct zenergy_data *data, int core)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (topology_physical_package_id(cpu) ==
				data->cores[core].package_id &&
		    topology_core_id(cpu) == data->cores[core].core_id)
			return cpu;
	}

	return -ENODEV;
}

static int zenergy_read_labels(struct device *dev,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel,
				  const char **str)
{
	struct zenergy_data *data = dev_get_drvdata(dev);

	*str = data->label[channel];
	return 0;
}

static int get_energy_units(struct zenergy_data *data)
{
	u64 rapl_units;
	int ret;

#if KERNEL_VERSION(6, 16, 0) <= LINUX_VERSION_CODE
	ret = rdmsrq_safe(ENERGY_PWR_UNIT_MSR, &rapl_units);
#else
	ret = rdmsrl_safe(ENERGY_PWR_UNIT_MSR, &rapl_units);
#endif
	if (ret)
		return ret;

	data->energy_units = (rapl_units & zenergy_UNIT_MASK) >> 8;
	return 0;
}

static int read_energy_msr(int cpu, u32 reg, u64 *value)
{
#if KERNEL_VERSION(6, 16, 0) <= LINUX_VERSION_CODE
	return rdmsrq_safe_on_cpu(cpu, reg, value);
#else
	return rdmsrl_safe_on_cpu(cpu, reg, value);
#endif
}

static int __accumulate_delta(struct sensor_accumulator *accum,
			      int cpu, u32 reg)
{
	u64 input;
	int ret;

	ret = read_energy_msr(cpu, reg, &input);
	if (ret)
		return ret;

	input &= zenergy_MASK;

	if (input >= accum->prev_value)
		accum->energy_ctr +=
			input - accum->prev_value;
	else
		accum->energy_ctr += UINT_MAX -
			accum->prev_value + input + 1;

	accum->prev_value = input;
	accum->cache_timeout = jiffies + HZ + (get_random_long() % HZ);
	return 0;
}

static int accumulate_delta(struct zenergy_data *data,
			    int channel, int cpu, u32 reg)
{
	int ret;

	mutex_lock(&data->lock);
	ret = __accumulate_delta(&data->accums[channel], cpu, reg);
	mutex_unlock(&data->lock);
	return ret;
}

static void read_accumulate(struct zenergy_data *data)
{
	int sock, scpu, cpu;
	int ret;

	for (sock = 0; sock < data->nr_socks; sock++) {
		scpu = zenergy_socket_cpu(data, sock);
		if (scpu < 0)
			continue;

		ret = accumulate_delta(data, data->nr_cpus + sock,
				       scpu, ENERGY_PKG_MSR);
		if (ret)
			pr_warn_ratelimited("%s: failed to read MSR 0x%x on CPU %d: %d\n",
					    DRVNAME, ENERGY_PKG_MSR, scpu, ret);
	}

	if (data->core_id >= data->nr_cpus)
		data->core_id = 0;

	cpu = zenergy_core_cpu(data, data->core_id);
	if (cpu >= 0) {
		ret = accumulate_delta(data, data->core_id, cpu, ENERGY_CORE_MSR);
		if (ret)
			pr_warn_ratelimited("%s: failed to read MSR 0x%x on CPU %d: %d\n",
					    DRVNAME, ENERGY_CORE_MSR, cpu, ret);
	}

	data->core_id++;
}

static int zenergy_read(struct device *dev,
			   enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct zenergy_data *data = dev_get_drvdata(dev);
	struct sensor_accumulator *accum;
	u64 energy;
	u32 reg;
	int cpu;
	int ret = 0;

	if (channel >= data->nr_cpus) {
		cpu = zenergy_socket_cpu(data, channel - data->nr_cpus);
		if (cpu < 0)
			return cpu;
		reg = ENERGY_PKG_MSR;
	} else {
		cpu = zenergy_core_cpu(data, channel);
		if (cpu < 0)
			return cpu;

		reg = ENERGY_CORE_MSR;
	}

	accum = &data->accums[channel];

	mutex_lock(&data->lock);
	if (data->do_not_accum) {
		ret = read_energy_msr(cpu, reg, &energy);
	} else {
		if (!accum->energy_ctr || time_after(jiffies, accum->cache_timeout))
			ret = __accumulate_delta(accum, cpu, reg);
		energy = accum->energy_ctr;
	}
	mutex_unlock(&data->lock);
	if (ret)
		return ret;

	*val = mul_u64_u64_div_u64(energy, 1000000,
				   BIT(data->energy_units));

	return 0;
}

static umode_t zenergy_is_visible(const void *_data,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel)
{
	return 0444;
}

static int energy_accumulator(void *p)
{
	struct zenergy_data *data = (struct zenergy_data *)p;
	unsigned int timeout = data->timeout_ms;

	while (!kthread_should_stop()) {
		/*
		 * Ignoring the conditions such as
		 * cpu being offline or rdmsr failure
		 */
		read_accumulate(data);

		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;

		schedule_timeout(msecs_to_jiffies(timeout));
	}
	return 0;
}

static const struct hwmon_ops zenergy_ops = {
	.is_visible = zenergy_is_visible,
	.read = zenergy_read,
	.read_string = zenergy_read_labels,
};

static int amd_create_sensor(struct device *dev,
			     struct zenergy_data *data,
			     enum hwmon_sensor_types type, u32 config)
{
	struct hwmon_channel_info *info = &data->energy_info;
	struct sensor_accumulator *accums;
	struct zenergy_core *cores;
	int i, cpu, package_id, core_id, cpus, sockets, max_cpus;
	int *socket_ids;
	u32 *s_config;
	char (*label_l)[10];

	max_cpus = num_present_cpus();
	if (!max_cpus)
		return -ENODEV;

	socket_ids = devm_kcalloc(dev, max_cpus,
				 sizeof(*socket_ids), GFP_KERNEL);
	if (!socket_ids)
		return -ENOMEM;
	cores = devm_kcalloc(dev, max_cpus, sizeof(*cores), GFP_KERNEL);
	if (!cores)
		return -ENOMEM;

	cpus = 0;
	sockets = 0;
	for_each_present_cpu(cpu) {
		package_id = topology_physical_package_id(cpu);
		core_id = topology_core_id(cpu);
		if (package_id < 0 || core_id < 0)
			return -EINVAL;

		for (i = 0; i < sockets; i++) {
			if (socket_ids[i] == package_id)
				break;
		}
		if (i == sockets)
			socket_ids[sockets++] = package_id;

		for (i = 0; i < cpus; i++) {
			if (cores[i].package_id == package_id &&
			    cores[i].core_id == core_id)
				break;
		}
		if (i == cpus) {
			cores[cpus].package_id = package_id;
			cores[cpus].core_id = core_id;
			cpus++;
		}
	}
	if (!sockets || !cpus)
		return -ENODEV;

	s_config = devm_kcalloc(dev, cpus + sockets + 1,
				sizeof(u32), GFP_KERNEL);
	if (!s_config)
		return -ENOMEM;

	accums = devm_kcalloc(dev, cpus + sockets,
			      sizeof(struct sensor_accumulator),
			      GFP_KERNEL);
	if (!accums)
		return -ENOMEM;

	label_l = devm_kcalloc(dev, cpus + sockets,
			       sizeof(*label_l), GFP_KERNEL);
	if (!label_l)
		return -ENOMEM;

	info->type = type;
	info->config = s_config;

	data->nr_cpus = cpus;
	data->nr_socks = sockets;
	data->socket_ids = socket_ids;
	data->cores = cores;
	data->accums = accums;
	data->label = label_l;

	for (i = 0; i < cpus + sockets; i++) {
		s_config[i] = config;
		if (i < cpus)
			scnprintf(label_l[i], 10, "Ecore%03u", i);
		else
			scnprintf(label_l[i], 10, "Esocket%u", (i - cpus));
	}

	s_config[i] = 0;
	return 0;
}

static const struct x86_cpu_id bit32_rapl_cpus[] = {
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x01, NULL),	/* Zen */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x08, NULL),	/* Zen+ */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x11, NULL),	/* Zen APU */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x18, NULL),	/* Picasso */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x20, NULL),	/* Picasso APU */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x31, NULL),	/* Zen2 Threadripper */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x60, NULL),	/* Renoir */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x68, NULL),	/* Lucienne */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x71, NULL),	/* Zen2 */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x01, NULL),	/* Zen3 Threadripper */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x21, NULL),	/* Zen3 */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x50, NULL),	/* Cezanne */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x44, NULL),	/* Rembrandt */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x60, NULL),	/* Rembrandt */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x1A, 0x44, NULL),	/* Granite Ridge */
	// Zen4 (0x19, 0x61) features 64-bit registers for both Core::X86::Msr::CORE_ENERGY_STAT & L3::L3CT::L3PackageEnergyStatus),
	// c.f., https://www.amd.com/content/dam/amd/en/documents/processor-tech-docs/programmer-references/56713-B1_3_05.zip
	{}
};

static int zenergy_probe(struct platform_device *pdev)
{
	struct device *hwmon_dev;
	struct zenergy_data *data;
	struct device *dev = &pdev->dev;
	int ret;

	data = devm_kzalloc(dev,
			    sizeof(struct zenergy_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->chip.ops = &zenergy_ops;
	data->chip.info = data->info;

	dev_set_drvdata(dev, data);
	/* Populate per-core energy reporting */
	data->info[0] = &data->energy_info;
	ret = amd_create_sensor(dev, data, hwmon_energy,
				HWMON_E_INPUT | HWMON_E_LABEL);
	if (ret)
		return ret;

	mutex_init(&data->lock);
	ret = get_energy_units(data);
	if (ret) {
		dev_err(dev, "failed to read energy unit MSR: %d\n", ret);
		return ret;
	}

	hwmon_dev = devm_hwmon_device_register_with_info(dev, DRVNAME,
							 data,
							 &data->chip,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	/*
	 * On a system with peak wattage of 250W
	 * timeout = 2 ^ 32 / 2 ^ energy_units / 250 secs
	 */
	data->timeout_ms = 1000 *
			   BIT(min(28, 31 - data->energy_units)) / 250;

	/*
	 * For AMD platforms with 64-bit RAPL MSR registers, accumulation
	 * of the energy counters are not necessary.
	 */
	if (!x86_match_cpu(bit32_rapl_cpus)) {
		data->do_not_accum = true;
		pr_info("CPU supports 64-bit RAPL MSR registers\n");
		return 0;
	}

	data->wrap_accumulate = kthread_run(energy_accumulator, data,
					    "%s", dev_name(hwmon_dev));
	return PTR_ERR_OR_ZERO(data->wrap_accumulate);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
static int zenergy_remove(struct platform_device *pdev)
#else
static void zenergy_remove(struct platform_device *pdev)
#endif
{
	struct zenergy_data *data = dev_get_drvdata(&pdev->dev);

	if (data && data->wrap_accumulate)
		kthread_stop(data->wrap_accumulate);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
	return 0;
#endif
}

static const struct platform_device_id zenergy_ids[] = {
	{ .name = DRVNAME, },
	{}
};
MODULE_DEVICE_TABLE(platform, zenergy_ids);

static struct platform_driver zenergy_driver = {
	.probe = zenergy_probe,
	.remove	= zenergy_remove,
	.id_table = zenergy_ids,
	.driver = {
		.name = DRVNAME,
	},
};

static struct platform_device *zenergy_platdev;

static const struct x86_cpu_id cpu_ids[] __initconst = {
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x01, NULL),	/* Zen */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x08, NULL),	/* Zen+ */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x11, NULL),	/* Zen APU */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x18, NULL),	/* Picasso */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x20, NULL),	/* Picasso APU */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x31, NULL),	/* Zen2 Threadripper */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x60, NULL),	/* Renoir */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x68, NULL),	/* Lucienne */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x17, 0x71, NULL),	/* Zen2 */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x01, NULL),	/* Zen3 Threadripper */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x21, NULL),	/* Zen3 */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x50, NULL),	/* Cezanne */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x44, NULL),	/* Rembrandt */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x60, NULL),	/* Rembrandt */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x19, 0x61, NULL),	/* Zen 4 */
	X86_MATCH_VENDOR_FAM_MODEL(AMD, 0x1A, 0x44, NULL),	/* Zen 5 */
	{}
};
MODULE_DEVICE_TABLE(x86cpu, cpu_ids);

static int __init zenergy_init(void)
{
	int ret;

	if (!x86_match_cpu(cpu_ids))
		return -ENODEV;

	ret = platform_driver_register(&zenergy_driver);
	if (ret)
		return ret;

	zenergy_platdev = platform_device_alloc(DRVNAME, 0);
	if (!zenergy_platdev) {
		platform_driver_unregister(&zenergy_driver);
		return -ENOMEM;
	}

	ret = platform_device_add(zenergy_platdev);
	if (ret) {
		platform_device_put(zenergy_platdev);
		platform_driver_unregister(&zenergy_driver);
		return ret;
	}

	return ret;
}

static void __exit zenergy_exit(void)
{
	platform_device_unregister(zenergy_platdev);
	platform_driver_unregister(&zenergy_driver);
}

module_init(zenergy_init);
module_exit(zenergy_exit);

MODULE_DESCRIPTION("Driver for AMD Energy reporting from RAPL MSR via HWMON interface");
MODULE_AUTHOR("Naveen Krishna Chatradhi <nchatrad@amd.com>");
MODULE_LICENSE("GPL");

#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/delay.h>

#include "io.h"

static const struct i2c_device_id r2ec_id[] = {
	{ "stm32v1", 37 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, r2ec_id);

static const struct of_device_id r2ec_of_table[] = {
	{ .compatible = "tlt,stm32v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, r2ec_of_table);

struct r2ec {
	struct gpio_chip chip;
	struct irq_chip irqchip;
	struct i2c_client *client;
	struct mutex i2c_lock;
	struct mutex irq_lock;
};

struct r2ec_platform_data {
	unsigned gpio_base;

	int (*setup)(struct i2c_client *client, int gpio, unsigned ngpio,
		     void *context);

	int (*teardown)(struct i2c_client *client, int gpio, unsigned ngpio,
			void *context);

	void *context;
};

struct i2c_request {
	uint8_t version;
	uint16_t length;
	uint8_t command;
	uint8_t data[1];
	// uint8_t checksum; // invisible
} __attribute__((packed));

struct i2c_response {
	uint8_t version;
	uint8_t length;
	uint8_t command;
	uint8_t data[7];
	uint8_t checksum;
} __attribute__((packed));

static uint8_t calc_crc8(const uint8_t *data, size_t len)
{
	uint8_t crc = 0xFF;
	int i, j;

	for (j = 0; j < len; j++) {
		crc ^= data[j];

		for (i = 0; i < 8; i++) {
			crc = (crc & 0x80) ? (crc ^ 0xD5) << 1 : crc << 1;
		}
	}

	return crc;
}

// generate outcoming mesage checksum and write i2c data
static int stm32_write(struct i2c_client *client, uint8_t cmd,
		       uint8_t *data, size_t len)
{
	struct i2c_request *req;
	uint8_t tmp[sizeof(struct i2c_request) + len];
	const int tmp_len = sizeof(tmp);
	int err;

	if (!client) {
		printk(KERN_ERR "R2EC I2C client is not ready!\n");
		return -ENXIO;
	}

	req = (struct i2c_request *)tmp;
	req->version = PROTO_VERSION;
	req->length  = 2 + len; // 2 + data_len
	req->command = cmd;

	memcpy(req->data, data, len);

	req->data[len] = calc_crc8(tmp, tmp_len - 1);

	if ((err = i2c_master_send(client, tmp, tmp_len)) < 0) {
		return err;
	}

	return 0;
}

// attempt to read i2c data
static int stm32_read(struct i2c_client *client, uint8_t *data, size_t len)
{
	uint8_t checksum;
	int err;

	if (!client) {
		printk(KERN_ERR "R2EC I2C client is not ready!\n");
		return -ENXIO;
	}

	if ((err = i2c_master_recv(client, data, len)) < 0) {
		return err;
	}

	if (len == 1) {
		return 0;
	}

	// 0xFF - no data available
	if (*(data + 3) == 0xFF) {
		return -ENODATA;
	}

	// generate checksum and verify
	checksum = calc_crc8(data, len - 1);

	if (checksum != *(data + len - 1)) {
		dev_err(&client->dev, "Checksum of incoming message "
				      "does not match!\n");
		return -EBADE;
	}

	return 0;
}

// attempt to retrieve device state and boot into application state
// this is done without interrupt, so there should be delay after writing
//  request and before reading response
static int stm32_prepare(struct i2c_client *client)
{
	uint8_t data[1], recv[1];
	int ret;

	data[0] = BOOT_STATE;

	if ((ret = stm32_write(client, CMD_BOOT, data, 1))) {
		return ret;
	}

	if ((ret = stm32_read(client, recv, 1))) {
		return ret;
	}

	// handle the following possible states reported either from
	//  bootloader or system:
	switch (recv[0]) {
	case NO_IMAGE_FOUND:
	case APP_STARTED:
		return 0;
	case BOOT_STARTED:
	case WATCHDOG_RESET:
	case APPLICATION_START_FAIL:
	case HARD_FAULT_ERROR:
		break;
	default:
		dev_err(&client->dev, "Device did not responded with correct "
				      "state! Actual response was 0x%02X. "
				      "Unable to get device state!\n", recv[0]);
		break;
	}

	data[0] = BOOT_START_APP;

	if ((ret = stm32_write(client, CMD_BOOT, data, 1))) {
		return ret;
	}

	if ((ret = stm32_read(client, recv, 1))) {
		return ret;
	}

	if (recv[0] != STATUS_ACK) {
		dev_err(&client->dev, "Device did not responded with ACK. "
				      "Actual response was 0x%02X. "
				      "Unable to set device state!\n", recv[0]);
		return -EIO;
	}

	return 0;
}

static int stm32_gpio_write(struct i2c_client *client, int pin, int val)
{
	struct i2c_request *req;
	size_t len = 2;
	uint8_t tmp[sizeof(struct i2c_request) + len];
	int err;

	if (!client) {
		printk(KERN_ERR "R2EC I2C client is not ready!\n");
		return -ENXIO;
	}

	req = (struct i2c_request *)tmp;
	req->version = PROTO_VERSION;
	req->length  = 2 + len; // command + crc + data
	req->command = CMD_GPIO;
	req->data[0] = pin;
	req->data[1] = val;

	if ((err = i2c_master_send(client, tmp, sizeof(tmp))) < 0) {
		return err;
	}

	return 0;
}

static int stm32_gpio_read(struct i2c_client *client, int pin, int val)
{
	struct i2c_request *req;
	size_t len = 2;
	uint8_t tmp[sizeof(struct i2c_request) + len];
	uint8_t recv[1];
	int err;

	if (!client) {
		printk(KERN_ERR "R2EC I2C client is not ready!\n");
		return -ENXIO;
	}

	req = (struct i2c_request *)tmp;
	req->version = PROTO_VERSION;
	req->length  = 2 + len; // command + crc + data
	req->command = CMD_GPIO;
	req->data[0] = pin;
	req->data[1] = val;

	if ((err = i2c_master_send(client, tmp, sizeof(tmp))) < 0) {
		return err;
	}

	if ((err = i2c_master_recv(client, recv, sizeof(recv))) < 0) {
		return err;
	}

	switch (recv[0]) {
	case GPIO_STATE_HIGH:
		return 1;
	case GPIO_STATE_LOW:
		return 0;
	}

	return -EIO;
}

static int r2ec_get(struct gpio_chip *chip, unsigned offset)
{
	struct r2ec *gpio = gpiochip_get_data(chip);
	int value;

	mutex_lock(&gpio->i2c_lock);
	value = stm32_gpio_read(gpio->client, offset, GPIO_VALUE_GET);
	mutex_unlock(&gpio->i2c_lock);

	return value;
}

static void r2ec_set(struct gpio_chip *chip, unsigned offset, int value)
{
	struct r2ec *gpio = gpiochip_get_data(chip);
	int val = value ? GPIO_VALUE_SET_HIGH : GPIO_VALUE_SET_LOW;

	mutex_lock(&gpio->i2c_lock);
	stm32_gpio_write(gpio->client, offset, val);
	mutex_unlock(&gpio->i2c_lock);
}

static int r2ec_input(struct gpio_chip *chip, unsigned offset)
{
	struct r2ec *gpio = gpiochip_get_data(chip);
	int status;

	mutex_lock(&gpio->i2c_lock);
	status = stm32_gpio_write(gpio->client, offset, GPIO_MODE_SET_INPUT);
	mutex_unlock(&gpio->i2c_lock);

	return status;
}

static int r2ec_output(struct gpio_chip *chip, unsigned offset, int value)
{
	struct r2ec *gpio = gpiochip_get_data(chip);
	int status;

	mutex_lock(&gpio->i2c_lock);
	status = stm32_gpio_write(gpio->client, offset, GPIO_MODE_SET_OUTPUT);
	mutex_unlock(&gpio->i2c_lock);

	r2ec_set(chip, offset, value);

	return status;
}

static irqreturn_t r2ec_irq(int irq, void *data)
{
	struct r2ec *gpio = data;
	unsigned i;

	for (i = 0; i < gpio->chip.ngpio; i++) {
		handle_nested_irq(irq_find_mapping(gpio->chip.irq.domain, i));
	}

	return IRQ_HANDLED;
}

static void r2ec_irq_bus_lock(struct irq_data *data)
{
	struct r2ec *gpio = irq_data_get_irq_chip_data(data);
	mutex_lock(&gpio->irq_lock);
}

static void r2ec_irq_bus_sync_unlock(struct irq_data *data)
{
	struct r2ec *gpio = irq_data_get_irq_chip_data(data);
	mutex_unlock(&gpio->irq_lock);
}

static int r2ec_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct r2ec_platform_data *pdata = dev_get_platdata(&client->dev);
	struct r2ec *gpio;
	int status;

	if ((status = stm32_prepare(client))) {
		dev_err(&client->dev, "Unable to initialize device!\n");
		return status;
	}

	gpio = devm_kzalloc(&client->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio) {
		return -ENOMEM;
	}

	mutex_init(&gpio->irq_lock);
	mutex_init(&gpio->i2c_lock);

	lockdep_set_subclass(&gpio->i2c_lock,
			     i2c_adapter_depth(client->adapter));

	gpio->chip.base = pdata ? pdata->gpio_base : -1;
	gpio->chip.can_sleep = true;
	gpio->chip.parent = &client->dev;
	gpio->chip.owner = THIS_MODULE;
	gpio->chip.get = r2ec_get;
	gpio->chip.set = r2ec_set;
	gpio->chip.direction_input = r2ec_input;
	gpio->chip.direction_output = r2ec_output;
	gpio->chip.ngpio = id->driver_data;
	gpio->chip.label = client->name;
	gpio->client = client;

	i2c_set_clientdata(client, gpio);

	status = devm_gpiochip_add_data(&client->dev, &gpio->chip, gpio);
	if (status < 0) {
		goto fail;
	}

	if (client->irq) {
		gpio->irqchip.name = "r2ec";
		gpio->irqchip.irq_bus_lock = r2ec_irq_bus_lock;
		gpio->irqchip.irq_bus_sync_unlock = r2ec_irq_bus_sync_unlock;

		status = gpiochip_irqchip_add_nested(&gpio->chip,
						     &gpio->irqchip,
						     0, handle_level_irq,
						     IRQ_TYPE_NONE);
		if (status) {
			dev_err(&client->dev, "cannot add irqchip\n");
			goto fail;
		}

		status = devm_request_threaded_irq(&client->dev, client->irq,
						   NULL, r2ec_irq,
						   IRQF_ONESHOT |
						   IRQF_TRIGGER_FALLING |
						   IRQF_SHARED,
						   dev_name(&client->dev),
						   gpio);
		if (status) {
			goto fail;
		}
	}

	if (pdata && pdata->setup) {
		status = pdata->setup(client, gpio->chip.base, gpio->chip.ngpio,
				      pdata->context);

		if (status < 0) {
			dev_warn(&client->dev, "setup --> %d\n", status);
		}
	}

	dev_info(&client->dev, "probed\n");
	return 0;

fail:
	dev_dbg(&client->dev, "probe error %d for %s\n", status, client->name);
	return status;
}

static int r2ec_remove(struct i2c_client *client)
{
	struct r2ec_platform_data *pdata = dev_get_platdata(&client->dev);
	struct r2ec *gpio = i2c_get_clientdata(client);
	int status = 0;

	if (!(pdata && pdata->teardown)) {
		return status;
	}

	status = pdata->teardown(client, gpio->chip.base, gpio->chip.ngpio,
				 pdata->context);

	if (status < 0) {
		dev_err(&client->dev, "%s --> %d\n", "teardown", status);
	}

	return status;
}

static struct i2c_driver r2ec_driver = {
	.driver = {
		.name	= "r2ec",
		.of_match_table = of_match_ptr(r2ec_of_table),
	},
	.probe	= r2ec_probe,
	.remove	= r2ec_remove,
	.id_table = r2ec_id,
};

static int chip_label_match(struct gpio_chip *chip, void *data)
{
	return !strcmp(chip->label, data);
}

static int get_stm32_version(struct device *dev, uint8_t type, char *buffer)
{
	struct gpio_chip *chip;
	struct r2ec *gpio;
	uint8_t recv[sizeof(struct i2c_response)];
	uint8_t data[1];

	struct pt_fw_get_ver {
		unsigned char command_ex;
		unsigned char major;
		unsigned char middle;
		unsigned char minor;
		unsigned char rev;
	} __attribute__((packed)) *res;

	chip = gpiochip_find("stm32v1", chip_label_match);
	if (!chip) {
		printk(KERN_ERR "Unable to find R2EC gpio chip!\n");
		return -ENXIO;
	}

	gpio = gpiochip_get_data(chip);

	if (!gpio->client) {
		printk(KERN_ERR "R2EC I2C client is not ready!\n");
		return -ENXIO;
	}

	data[0] = (type == CMD_FW) ? FW_VERSION : BOOT_VERSION;

	mutex_lock(&gpio->i2c_lock);

	if (stm32_write(gpio->client, type, data, 1)) {
		printk(KERN_ERR "Unable transmit R2EC data!\n");
		goto done;
	}

	if (stm32_read(gpio->client, recv, sizeof(recv))) {
		printk(KERN_ERR "Unable receive R2EC data!\n");
		goto done;
	}

	res = (struct pt_fw_get_ver *)(&recv[3]);

	sprintf(buffer, "%02d.%02d.%02d rev. %02d\n",
		res->major, res->middle, res->minor, res->rev);

done:
	mutex_unlock(&gpio->i2c_lock);
	return strlen(buffer);
}

static ssize_t app_version_show(struct device *dev,
				struct device_attribute *attr, char *buffer)
{
	return get_stm32_version(dev, CMD_FW, buffer);
}

static ssize_t boot_version_show(struct device *dev,
				 struct device_attribute *attr, char *buffer)
{
	return get_stm32_version(dev, CMD_BOOT, buffer);
}

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buff, size_t count)
{
	struct gpio_chip *chip;
	struct r2ec *gpio;
	uint8_t data[1];

	chip = gpiochip_find("stm32v1", chip_label_match);
	if (!chip) {
		printk(KERN_ERR "Unable to find R2EC gpio chip!\n");
		return -ENXIO;
	}

	gpio = gpiochip_get_data(chip);

	if (!gpio->client) {
		printk(KERN_ERR "R2EC I2C client is not ready!\n");
		return -ENXIO;
	}

	data[0] = BOOT_START_APP;

	mutex_lock(&gpio->i2c_lock);
	if (stm32_write(gpio->client, CMD_BOOT, data, 1)) {
		printk(KERN_ERR "Unable transmit R2EC data!\n");
		goto done;
	}

done:
	mutex_unlock(&gpio->i2c_lock);
	return 1;
}

static struct device_attribute g_r2ec_kobj_attr[] = {
	__ATTR_RO(app_version),
	__ATTR_RO(boot_version),
	__ATTR_WO(reset)
};

static struct attribute *g_r2ec_attrs[] = {
	&g_r2ec_kobj_attr[0].attr,
	&g_r2ec_kobj_attr[1].attr,
	&g_r2ec_kobj_attr[2].attr,
	NULL,
};

static struct attribute_group g_r2ec_attr_group = { .attrs = g_r2ec_attrs };
static struct kobject *g_r2ec_kobj;

static int __init r2ec_init(void)
{
	int ret;

	ret = i2c_add_driver(&r2ec_driver);

	if (ret) {
		printk(KERN_ERR "Unable to initialize `r2ec` driver!\n");
		return ret;
	}

	g_r2ec_kobj = kobject_create_and_add("r2ec", NULL);
	if (!g_r2ec_kobj) {
		i2c_del_driver(&r2ec_driver);
		printk(KERN_ERR "Unable to create `r2ec` kobject!\n");
		return -ENOMEM;
	}

	if (sysfs_create_group(g_r2ec_kobj, &g_r2ec_attr_group)) {
		kobject_put(g_r2ec_kobj);
		i2c_del_driver(&r2ec_driver);
		printk(KERN_ERR "Unable to create `r2ec` sysfs group!\n");
	}

	return 0;
}

static void __exit r2ec_exit(void)
{
	kobject_put(g_r2ec_kobj);
	i2c_del_driver(&r2ec_driver);
}

module_init(r2ec_init);
module_exit(r2ec_exit);

MODULE_AUTHOR("Jokubas Maciulaitis <jokubas.maciulaitis@teltonika.lt>");
MODULE_DESCRIPTION("STM32F0 (R2EC) I2C GPIO Expander driver");
MODULE_LICENSE("GPL v2");

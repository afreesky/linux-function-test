#include <linux/debugfs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>

#include "common.h"

struct dentry *debugfs_dir;


ssize_t debugfs_read_suspend_count(struct file *file, char __user *buf,
                                 size_t count, loff_t *ppos)
{
    struct my_driver_private *priv = file->private_data;
    char buffer[32];
    int len;

    len = snprintf(buffer, sizeof(buffer), "%d\n", priv->is_suspended ? 1 : 0);

    return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

ssize_t debugfs_write_suspend_count(struct file *file, const char __user *buf,
                                  size_t count, loff_t *ppos)
{
    struct my_driver_private *priv = file->private_data;
    char buffer[32];
    int ret;
    int suspend;

    if (count >= sizeof(buffer))
        return -EINVAL;

    if (copy_from_user(buffer, buf, count))
        return -EFAULT;

    buffer[count] = '\0';

    ret = sscanf(buffer, "%d", &suspend);
    if (ret != 1)
        return -EINVAL;

    // if (suspend) {
    //     my_device_suspend(priv->dev);
    // } else {
    //     my_device_resume(priv->dev);
    // }

    if (suspend) {
        pm_runtime_get_noresume(priv->dev);
    } else {
        pm_runtime_put_noidle(priv->dev);
    }

    return count;
}

static const struct file_operations debugfs_fops = {
    .read = debugfs_read_suspend_count,
    .write = debugfs_write_suspend_count,
    .owner = THIS_MODULE,
};

void create_debugfs_entries(struct my_driver_private *priv)
{
    debugfs_dir = debugfs_create_dir("my_device_debugfs", NULL);
    if (!debugfs_dir) {
        dev_err(priv->dev, "Failed to create debugfs directory\n");
        return;
    }

    debugfs_create_file("suspend_state", 0666, debugfs_dir, priv, &debugfs_fops);
}

void remove_debugfs_entries(void)
{
    debugfs_remove_recursive(debugfs_dir);
}
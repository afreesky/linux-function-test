#ifndef _COMMON_H_
#define _COMMON_H_


struct my_driver_private {
    void __iomem *reg_base;
    int irq_number;
    struct device *dev;
    bool is_suspended;
};

void create_debugfs_entries(struct my_driver_private *priv);
void remove_debugfs_entries(void);

#endif /* _COMMON_H_ */
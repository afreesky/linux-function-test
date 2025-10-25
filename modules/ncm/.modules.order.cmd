cmd_/home/alanliu/Projects/modules/ncm/modules.order := {   echo /home/alanliu/Projects/modules/ncm/ncm.ko; :; } | awk '!x[$$0]++' - > /home/alanliu/Projects/modules/ncm/modules.order

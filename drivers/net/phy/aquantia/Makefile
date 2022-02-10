obj-$(CONFIG_AQUANTIA_PHY)	+= aquantia.o

aquantia-objs := aquantia_main.o

ifdef CONFIG_HWMON
aquantia-objs			+= aquantia_hwmon.o
endif

ifdef CONFIG_AQUANTIA_MACSEC
aquantia-objs			+= macsec/macsec.o
aquantia-objs			+= aqr_macsec/aqr_macsec.o
endif

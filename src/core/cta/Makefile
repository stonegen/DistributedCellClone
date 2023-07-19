ROOT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
ifeq ($(RTE_SDK),)
$(error "Please define RTE_SDK environment variable")
endif

# Default target, can be overriden by command line or environment
RTE_TARGET ?= x86_64-native-linuxapp-gcc

include $(RTE_SDK)/mk/rte.vars.mk

# binary name
APP = cta

# Use g++ to compile the application
CC = g++

# all source are stored in SRCS-y
SRCS-y := main.c

# Include init files.
SRCS-y += $(ROOT)init/initializer.cpp
SRCS-y += $(ROOT)NicInterface/NicInterface.cpp

# Include Common libs
SRCS-y += $(ROOT)Common/time/time.cpp
SRCS-y += $(ROOT)Common/logger/Logger.cpp
SRCS-y += $(ROOT)Common/custom_header/CustomHeaderParser.cpp

# Include LB
SRCS-y += $(ROOT)load_balancer/controller.cpp
SRCS-y += $(ROOT)load_balancer/lbaas/load_balancer.cpp

# Include CPF
SRCS-y += $(ROOT)cpf/cpf.cpp

CFLAGS += -w
CFLAGS += -g
CFLAGS += -lpthread
CFLAGS += -O0
CFLAGS += -std=c++11
CFLAGS += -fpermissive

# CFLAGS += $(WERROR_FLAGS)
CFLAGS += -I$(ROOT)cpf/asn1/compiled
CFLAGS += -I$(ROOT)cpf/flatbuffer/generated
CFLAGS += -I$(ROOT)cpf/flatbuffer/include
CFLAGS += -W

LDFLAGS += -lconfig++

include $(RTE_SDK)/mk/rte.extapp.mk

set(FIND_PLANCKCLOCK_PATHS
        /usr)

find_path(PLANCKCLOCK_INCLUDE_DIR planckclock.h
        PATH_SUFFIXES include
        PATHS $(FIND_PLANCKCLOCK_PATHS))

find_library(PLANCKCLOCK_LIBRARY
        NAMES planckclocklib
        PATH_SUFFIXES lib
        PATHS $(FIND_PLANCKCLOCK_PATHS))
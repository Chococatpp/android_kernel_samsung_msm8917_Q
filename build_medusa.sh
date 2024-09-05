##############################################
#   SebaUbuntu custom kernel build script    #
##############################################

# Set defaults directory's
ROOT_DIR=$(pwd)
OUT_DIR=$ROOT_DIR/out
ANYKERNEL_DIR=$ROOT_DIR/anykernel3
KERNEL_DIR=$ROOT_DIR
DATE=$(date +"%m-%d-%y")
BUILD_START=$(date +"%s")

# Export ARCH and SUBARCH <arm, arm64, x86, x86_64>
export ARCH=arm
export SUBARCH=arm

# Set kernel name and defconfig
# export VERSION=
DEF=j4corelte_defconfig
export DEFCONFIG=$DEF

# Keep it as is
export LOCALVERSION=$VERSION

# Export Username and machine name
export KBUILD_BUILD_USER=Batu33TR
export KBUILD_BUILD_HOST=ProjectMedusa

# Color definition
red=`tput setaf 1`
green=`tput setaf 2`
yellow=`tput setaf 3`
blue=`tput setaf 4`
magenta=`tput setaf 5`
cyan=`tput setaf 6`
white=`tput setaf 7`
reset=`tput sgr0`

# Cross-compiler exporting
	# Export ARM from the given directory
	export CROSS_COMPILE=$(pwd)/gcc/bin/arm-linux-androideabi-

echo -e "*****************************************************"
echo    "            Compiling kernel using GCC               "
echo -e "*****************************************************"
echo -e "-----------------------------------------------------"
echo    " Architecture: $ARCH                                 "
echo    " Output directory: $OUT_DIR                          "
echo    " Kernel version: $VERSION                            "
echo	  " Defconfig: $DEF				                              "
echo    " Build user: $KBUILD_BUILD_USER                      "
echo    " Build machine: $KBUILD_BUILD_HOST                   "
echo    " Build started on: $BUILD_START                      "
echo    " Toolchain: GCC 4.9 Brillo-M10-Release               "
echo -e "-----------------------------------------------------"

# Set kernel source workspace
cd $KERNEL_DIR

# Clean build
rm -rf out

# Make your device device_defconfig
make O=$OUT_DIR ARCH=$ARCH KCFLAGS=-mno-android $DEFCONFIG
DEFCONFIG_SUCCESS=$?
if [ $DEFCONFIG_SUCCESS != 0 ]
	then
		echo "$red Error: make $DEFCONFIG failed, specified a defconfig not present? $reset"
		exit
fi

# Build Kernel
make O=$OUT_DIR ARCH=$ARCH KCFLAGS=-mno-android -j$(nproc --all)

# Find how much build has been long
BUILD_END=$(date +"%s")
DIFF=$(($BUILD_END - $BUILD_START))

BUILD_SUCCESS=$?
if [ $BUILD_SUCCESS != 0 ]
	then
		echo "$red Error: Build failed in $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds $reset"
		exit
fi


echo -e "$green Build completed in $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds $reset"

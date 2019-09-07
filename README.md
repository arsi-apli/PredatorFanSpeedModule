Acer Predator Helios 500 CPU Fan control linux kernel module
=======

Module to allow more aggressive CPU fan speed control settings

Verion 0.1 Beta
 
 The driver is tested on Predator PH517-51 bios V1.06 and kernel 5.1.21-050121-generic
 
 If you have another version of predator please first check if the EC registers also apply to your Predator.
 
 I searched the registers according to the following instructions:
 
 https://github.com/hirschmann/nbfc/wiki/Probe-the-EC%27s-registers
 
 ```
 Then add your predator to the variable: bios_settings 
 {"Acer", "Predator PH517-51", "V1.06", 0x4f, 0x58, {0x14, 0x04}, 1},
                   ^              ^       ^    ^    ^^^^^^^^^^^^^^^^^
                 Model           Bios    Fan  Temp   Unussed
```                 
 
 Please do not forget to do a PR to make life easier for others
 
 The current fancontrol table is hardcoded in function acerhdf_set_cur_state
 It is quite aggressive, you can adjust it according to your requirements.
 
 TODO:
 
       code cleanup
      
       filter fan speed by time

Installation:

make clean

make

sudo insmod acerhdf.ko

sudo rmmod acerhdf.ko


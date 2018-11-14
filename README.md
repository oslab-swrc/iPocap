# iPocap
iPocap is idle injection based power-capping using intel_powerclamp driver. 

# How to Use
After setting up POWERCLAMP_POWERCAP in configuration, build kernel and reboot.

Then, you can see the new path to control iPocap, /sys/powercapper

> There are four files, each of the roles is:
> * start: if you write 1 in this file, you can start iPocap
> * stop: if you write 1 in this file, you can stop iPocap
> * set_target: Write down the upper limit of the power(Watt) you want.
> * status: You can read the on/off status of the iPocap through this file.

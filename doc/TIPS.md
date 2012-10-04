aprsc tricks and tips
=====================


Providing access on low TCP ports (like 23)
----------------------------------------------

For security reasons aprsc drops root privileges as soon as possible after
starting up (if it ever had them in the first place).  Listening on
privileged "low" ports below 1024 normally requires root privileges, which
aprsc no longer has when it comes to the point where it would start binding
those ports.

You can use a NAT based method to redirect traffic from port 23 to port
14580 (or some other high unprivileged port your server is listening on). 
Replace *youripaddress* with your external IP address.  The local listening
address (to-destination) cannot be localhost, so use the same IP address. 
These two commands need to go somewhere in your startup scripts or firewall
configurations.

    root@box:~# iptables -t nat -A PREROUTING -d *youripaddress*
        -p tcp --dport 23 -m addrtype --dst-type LOCAL -j DNAT
        --to-destination *youripaddress*:14580

    root@box:~# iptables -t nat -A OUTPUT -d *youripaddress*
        -p tcp --dport 29 -m addrtype --dst-type LOCAL -j DNAT
        --to-destination *youripaddress*:14580


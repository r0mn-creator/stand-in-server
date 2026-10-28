@echo off

set RULE_NAME="Stand-In Server"

rem Delete the rule
netsh advfirewall firewall delete rule name=%RULE_NAME%

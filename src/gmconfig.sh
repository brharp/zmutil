#!/bin/sh

#
# Create  default domain and accounts.
#
/opt/zimbra/bin/zmprov <<ZMPROV
createDomain uoguelph.ca zimbraAuthLdapSearchBase 'ou=People,o=uoguelph.ca' zimbraAuthLdapSearchFilter '(uid=%u)' zimbraAuthLdapURL 'ldap://directory.uoguelph.ca:389' zimbraAuthLdapStartTlsEnabled 'TRUE' zimbraAuthMech 'ldap' zimbraSkinLogoURL 'http://www.uoguelph.ca/'
modifyConfig zimbraDefaultDomainName 'uoguelph.ca'
createAccount brharp@uoguelph.ca '' zimbraIsAdminAccount 'TRUE'
createAccount rofoster@uoguelph.ca '' zimbraIsAdminAccount 'TRUE'
createAccount spatara@uoguelph.ca '' zimbraIsAdminAccount 'TRUE'
createAccount ztest01@uoguelph.ca ''
createAccount ztest02@uoguelph.ca ''
createAccount ztest03@uoguelph.ca ''
createAccount ztest04@uoguelph.ca ''
createAccount ztest05@uoguelph.ca ''
createAccount ztest06@uoguelph.ca ''
createAccount ztest07@uoguelph.ca ''
createAccount ztest08@uoguelph.ca ''
createAccount ztest09@uoguelph.ca ''
createAccount ztest10@uoguelph.ca ''
ZMPROV

#
# Share Data Between Test Accounts
#
zmprov cdl ztest@uoguelph.ca

for (( i = 1 ; i <= 10 ; i++ ))
do
 printf "adlm ztest@uoguelph.ca ztest%02d@uoguelph.ca\n" $i
done |
zmprov

for (( i = 1 ; i <= 10 ; i++ ))
do
 printf "sm ztest%02d@uoguelph.ca\n" "${i}"
 printf "mfg /Calendar group ztest@uoguelph.ca r\n"
done |
zmmailbox -z

for (( i = 1 ; i <= 10 ; i++ ))
do
 printf "sm ztest%02d@uoguelph.ca\n" "${i}"
 for (( j = 1 ; j <= 10 ; j++ ))
 do
  if [ "${i}" != "${j}" ]
  then
   printf "cm --view appointment /ztest%02d ztest%02d@uoguelph.ca /Calendar\n" "${j}" "${j}"
  fi
 done
done |
zmmailbox -z


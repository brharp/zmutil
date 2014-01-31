if [ $# -lt 3 ]
then
	echo "Usage: $0 <owner> <resource> <displayname> [<password>]"
	exit 1
fi

owner=${1}
resource=${2}
display=${3}
password=${4}
domain=uoguelph.ca

zmprov createCalendarResource ${resource}@${domain} "${password}" displayName "${display}" zimbraCalResType "Equipment"
zmprov setAccountCos ${resource}@${domain} resource

for folder in /Inbox /Sent /Calendar
do
	zmmailbox -z -m ${resource}@${domain} modifyFolderGrant ${folder} account ${owner}@${domain} rwidxa
done

zmmailbox -z -m ${owner}@${domain} createMountpoint "/${display}'s Inbox" ${resource}@${domain} /Inbox
zmmailbox -z -m ${owner}@${domain} createMountpoint "/${display}'s Sent" ${resource}@${domain} /Sent
zmmailbox -z -m ${owner}@${domain} createMountpoint -V appointment "/${display}'s Calendar" ${resource}@${domain} /Calendar


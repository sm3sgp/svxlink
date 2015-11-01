#!/bin/sh

if [ -n "$HOSTAUDIO_GID" ]; then
  groupadd -g $HOSTAUDIO_GID hostaudio
  usermod -G $HOSTAUDIO_GID svxlink
fi

HOME=$(getent passwd svxlink | cut -d':' -f6)
cd $HOME
PATH=$PATH:/usr/lib64/qt4/bin

if [ $# -gt 0 ]; then
  exec su -p svxlink -c "$@"
else
  exec su -p svxlink
fi
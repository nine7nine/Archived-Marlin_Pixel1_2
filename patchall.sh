#!/bin/bash

echo "Applying all patches"

for p in *.patch; do
echo "$p"
patch -Np1 -i "$p"
echo "---"
done

echo "finished"

exit


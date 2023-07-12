mkdir $out
# cp -r $src/. $out
cd $src
echo $out
# touch ninja
# touch build.ninja
python3 configure.py --bootstrap

# mkdir bin
# cp -r ./ninja bin
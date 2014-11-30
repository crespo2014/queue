for i in *.grp
do
 dot -T png -o ${i/%.grp/.png} ${i}
done

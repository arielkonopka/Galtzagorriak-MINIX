# records.be: the tsv and pipe modules (spec K.1, R7c.3, issue #385).
# Reads the readings below as TSV (the -t format of the commands), keeps
# the warm rooms, sorts them by temperature, prints them back as TSV.
import tsv
import pipe
import sensor
var text = "name\ttemp_c\nkitchen\t21.5\nhall\t19\ncellar\t12.5\n"
var r = tsv.records(text)
var warm = pipe.where(r, def (x) return real(x["temp_c"]) > 15 end)
print(tsv.join(["name", "temp_c"]))
for x: pipe.sortby(warm, def (x) return real(x["temp_c"]) end)
  print(tsv.join([x["name"], x["temp_c"]]))
end
print(pipe.map(pipe.head(r, 2), def (x) return x["name"] end))
# The sensors that exist (none in QEMU) and a missing one (nil).
print(size(sensor.list()) >= 0, sensor.read("nope"))

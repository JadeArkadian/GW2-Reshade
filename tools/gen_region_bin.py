import urllib.request, json

f_out = open("region.bin", "wb")

url = "https://api.guildwars2.com/v2/maps"
response = urllib.request.urlopen(url)
data = json.loads(response.read())
barray = [0]*(data[-1] + 1)
print("Size : " + str(len(barray)))
for id in data:
    print("%.2f" % ((id/len(barray))*100) + "%", end="\r")
    map_url = url + "/" + str(id)
    map_response = urllib.request.urlopen(map_url)
    map_data = json.loads(map_response.read())
    if "region_id" not in map_data:
        continue
    barray[id] = map_data["region_id"]
f_out.write(bytearray(barray))
f_out.close()
print("Done.")

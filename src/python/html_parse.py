import json
import folium

def get_signal_color(rsrp):
    if rsrp >= -95:
        return 'green'
    elif rsrp >= -105:
        return 'orange' 
    else:
        return 'red'

def create_coverage_map(input_file, output_file):
    m = folium.Map(location=[55.0188, 82.9522], zoom_start=16, tiles='CartoDB voyager')
    with open(input_file, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
                
            try:
                data = json.loads(line)
                
                if 'location' not in data:
                    continue
                    
                lat = data['location']['_Latitude']
                lon = data['location']['_Longitude']
                if 'cells' in data and len(data['cells']) > 0:
                    serving_cell = data['cells'][0]
                    rsrp = serving_cell.get('rsrp')
                    pci = serving_cell.get('pci')
                    
                    if rsrp is not None:
                        color = get_signal_color(rsrp)
                        folium.CircleMarker(
                            location=[lat, lon],
                            radius=6,
                            popup=f"RSRP: {rsrp} dBm<br>PCI: {pci}",
                            color=color,
                            fill=True,
                            fill_color=color,
                            fill_opacity=0.8,
                            weight=1
                        ).add_to(m)
                        
            except json.JSONDecodeError:
                print("error parse")
                continue
    m.save(output_file)
    print(f"Successful : {output_file}")

if __name__ == "__main__":
    create_coverage_map('../build/Location_save_data/received_data.jsonl', '../build/map/map.html')
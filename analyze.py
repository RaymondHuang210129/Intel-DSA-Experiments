import matplotlib.pyplot as plt

f = open("side_channel.log", "r")
latencies = []
next(f)
next(f)
next(f)
next(f)
next(f)
x = 0
for line in f:
    if x >= 100:
        break
    print(line)
    line = line.strip()
    latencies.extend([int(num) for num in line.split(' ')])
    x += 1

print(len(latencies))

for i in range(0, 1600, 200):
    xpoint = [num for num in range(0, 200)]
    ypoint = latencies[i:i+200]

    plt.plot(xpoint, ypoint)
    ax = plt.gca()
    # ax.set_ylim([250, 400])

    plt.show()
    plt.savefig(f"result{i}.png")
    plt.close()

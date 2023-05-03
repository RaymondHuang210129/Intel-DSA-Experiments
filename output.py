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
    if x >= 1600:
        break
    print(line)
    line = line.strip()
    latencies.extend([int(num) for num in line.split(' ')])
    x += 1

print(len(latencies))

for i in range(0, 25600, 1600):
    xpoint = [num for num in range(0, 1600)]
    ypoint = latencies[i:i+1600]

    plt.figure().set_figwidth(15)
    plt.scatter(xpoint, ypoint, [1] * 1600)
    ax = plt.gca()
    ax.ticklabel_format(style='plain')
    ax.set_ylim([0, 7500])

    plt.show()
    plt.savefig(f"result{i}.png", dpi=800)
    plt.close()


const child_process = require("child_process");
const { exit } = require("process");

let N = 1000;

let i = 0;

setTimeout(() => {
    console.log(`${i} request sent`);
    exit();
}, 1000)


while (true) {

}
for(i = 0; i < N; i++) {
    setTimeout(() => {
        child_process.exec(`./07_client set k${i} v${i}`);
    }, 1000);
}

console.log(`${N} set sent`);

for(let i = 0; i < N; i++) {
    setTimeout(() => {
        child_process.exec(`./07_client get k${i}`, (error, stdout, stderr) => {
            if (error) {
                console.error(error);
                return;
            }
            if (stdout.trim() !== `server says: [0] v${i}`) {
                console.log(`something error: ${stdout}`);
            }
        });
    }, 2000);
}
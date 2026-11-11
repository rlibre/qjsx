// Imported module: classes, getters, static members, private fields.
export class Shape {
    static count = 0;
    #name;
    constructor(name) {
        this.#name = name;
        Shape.count++;
    }
    get name() {
        return this.#name;
    }
    area() {
        return 0;
    }
}

export class Rectangle extends Shape {
    constructor(width, height) {
        super("rectangle");
        this.width = width;
        this.height = height;
    }
    area() {
        return this.width * this.height;
    }
}

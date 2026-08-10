import { createApp } from "vue";
import { createPinia } from "pinia";
import App from "./App.vue";
import router from "./router";

import "@fortawesome/fontawesome-free/css/all.min.css";
import "uplot/dist/uPlot.min.css";
import "notyf/notyf.min.css";
import "./assets/styles/main.css";

const saved = localStorage.getItem("theme");
if (saved) {
  document.documentElement.setAttribute("data-theme", saved);
} else {
  const prefersDark = window.matchMedia("(prefers-color-scheme: dark)").matches;
  document.documentElement.setAttribute("data-theme", prefersDark ? "dark" : "light");
}

createApp(App).use(createPinia()).use(router).mount("#app");
